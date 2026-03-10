#define SDL_MAIN_HANDLED
#include "controllers.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include <string>
#include <thread>
#include <atomic>

// Spotify helpers
static HWND FindSpotifyWindow()
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe))
            do {
                if (_wcsicmp(pe.szExeFile, L"Spotify.exe") == 0) { pid = pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
    }
    if (!pid) return nullptr;

    struct FD { DWORD pid; HWND hwnd; } fd{ pid, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* fd = reinterpret_cast<FD*>(lp);
        DWORD p = 0; GetWindowThreadProcessId(hwnd, &p);
        if (p == fd->pid && IsWindowVisible(hwnd))
        {
            wchar_t cls[256]{};
            GetClassNameW(hwnd, cls, 256);
            if (wcsstr(cls, L"Chrome_WidgetWin")) { fd->hwnd = hwnd; return FALSE; }
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&fd));
    return fd.hwnd;
}

static void SpotifyNext() { HWND hw = FindSpotifyWindow(); if (hw) SendMessage(hw, WM_APPCOMMAND, (WPARAM)hw, MAKELPARAM(0, FAPPCOMMAND_KEY | APPCOMMAND_MEDIA_NEXTTRACK)); }
static void SpotifyPrevious() { HWND hw = FindSpotifyWindow(); if (hw) SendMessage(hw, WM_APPCOMMAND, (WPARAM)hw, MAKELPARAM(0, FAPPCOMMAND_KEY | APPCOMMAND_MEDIA_PREVIOUSTRACK)); }

// Bindings
enum ActionID { ACTION_NEXT = 0, ACTION_PREV = 1, ACTION_COUNT = 2 };
static const char* ActionNames[ACTION_COUNT] = { "Next Track", "Previous Track" };
struct Binding { bool bound = false; int button = -1; };

static Binding             g_bindings[ACTION_COUNT];
static std::atomic<int>    g_listeningFor{ -1 };
static std::atomic<Uint32> g_listenStartTime{ 0 };
static ControllerType      g_controllerType{ ControllerType::Unknown };
static std::string         g_joystickName;
static ImFont* g_fontStatus = nullptr;

// Config — %APPDATA%\DS4SpotifyController\binds.cfg
static std::string GetConfigPath()
{
    char path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
    {
        std::string dir = std::string(path) + "\\DS4SpotifyController";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\binds.cfg";
    }
    return "binds.cfg";
}

static void SaveBindings()
{
    FILE* f = nullptr;
    fopen_s(&f, GetConfigPath().c_str(), "w");
    if (!f) return;
    for (int a = 0; a < ACTION_COUNT; ++a)
        fprintf(f, "%d=%d\n", a, g_bindings[a].bound ? g_bindings[a].button : -1);
    fclose(f);
}

static void LoadBindings()
{
    FILE* f = nullptr;
    fopen_s(&f, GetConfigPath().c_str(), "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f))
    {
        int a = -1, b = -1;
        if (sscanf_s(line, "%d=%d", &a, &b) == 2 && a >= 0 && a < ACTION_COUNT)
        {
            g_bindings[a].button = b;
            g_bindings[a].bound = (b >= 0);
        }
    }
    fclose(f);
}

// System tray — minimize hides to tray, click restores
#define WM_TRAYICON (WM_USER + 1)
#define TRAY_ID     1

static NOTIFYICONDATAA g_nid{};
static HWND            g_hwnd = nullptr;
static WNDPROC         g_origWndProc = nullptr;

static void TrayAdd(HWND hwnd)
{
    g_hwnd = hwnd;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    strncpy_s(g_nid.szTip, sizeof(g_nid.szTip), "DS4 Spotify Controller", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void TrayRemove() { Shell_NotifyIconA(NIM_DELETE, &g_nid); }

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE)
    {
        ShowWindow(hwnd, SW_HIDE); return 0;
    }
    if (msg == WM_TRAYICON && (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP))
    {
        ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); return 0;
    }
    return CallWindowProc(g_origWndProc, hwnd, msg, wp, lp);
}

// Controller thread
// Xbox uses SDL_GameController (XInput), DS4/DS5 use raw SDL_Joystick (HID).
// Connect/disconnect signals come from main thread via atomics.
static std::atomic<bool> g_running{ true };
static std::atomic<int>  g_deviceAdded{ -1 };
static std::atomic<int>  g_deviceRemoved{ -1 };

static void ControllerThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    SDL_Joystick* joystick = nullptr;
    SDL_GameController* gamepad = nullptr;
    SDL_JoystickID      instanceID = -1;
    bool                useGamepad = false;

    auto closeController = [&]() {
        if (gamepad) { SDL_GameControllerClose(gamepad); gamepad = nullptr; }
        if (joystick) { SDL_JoystickClose(joystick);      joystick = nullptr; }
        instanceID = -1; useGamepad = false;
        g_controllerType = ControllerType::Unknown;
        g_joystickName.clear();
        };

    auto openController = [&](int idx) {
        closeController();
        const char* name = SDL_JoystickNameForIndex(idx);
        g_joystickName = name ? name : "";
        g_controllerType = DetectControllerType(name);

        // DS4Windows remaps DS4 as Xbox — override via USB vendor/product GUID
        // Vendor 0x054C = Sony, Product 0x0CE6 = DS5, others = DS4
        if (g_controllerType == ControllerType::XboxOne)
        {
            SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(idx);
            Uint16 vendor = (Uint16)(guid.data[4] | (guid.data[5] << 8));
            Uint16 product = (Uint16)(guid.data[8] | (guid.data[9] << 8));
            if (vendor == 0x054C)
                g_controllerType = (product == 0x0CE6) ? ControllerType::DS5 : ControllerType::DS4;
        }

        if (g_controllerType == ControllerType::XboxOne && SDL_IsGameController(idx))
        {
            gamepad = SDL_GameControllerOpen(idx);
            useGamepad = true;
            if (gamepad) instanceID = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
        }
        else
        {
            joystick = SDL_JoystickOpen(idx);
            useGamepad = false;
            if (joystick) instanceID = SDL_JoystickInstanceID(joystick);
        }
        };

    if (SDL_NumJoysticks() > 0) openController(0);

    bool prevJoy[64] = {};
    bool prevPad[SDL_CONTROLLER_BUTTON_MAX] = {};
    bool prevJoyAxis[16] = {};
    bool prevPadAxis[SDL_CONTROLLER_AXIS_MAX] = {};
    constexpr Sint16 TRIGGER_THRESHOLD = 16384;
    constexpr int    AXIS_BUTTON_OFFSET = 1000;

    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG ticksPerPoll = freq.QuadPart / 250; // 4ms

    while (g_running)
    {
        // Connect signal — scan for the device with an instance ID we don't have open
        int addIdx = g_deviceAdded.exchange(-1);
        if (addIdx >= 0)
        {
            int n = SDL_NumJoysticks(), newIdx = -1;
            for (int i = 0; i < n; ++i)
            {
                SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
                if (id != instanceID || id == -1) { newIdx = i; break; }
            }
            if (newIdx >= 0) openController(newIdx);
        }

        // Disconnect signal
        int remID = g_deviceRemoved.exchange(-1);
        if (remID >= 0 && (remID == instanceID || SDL_NumJoysticks() == 0))
        {
            closeController();
            memset(prevJoy, 0, sizeof(prevJoy));
            memset(prevPad, 0, sizeof(prevPad));
            memset(prevJoyAxis, 0, sizeof(prevJoyAxis));
            memset(prevPadAxis, 0, sizeof(prevPadAxis));
        }

        if (instanceID == -1) { Sleep(100); QueryPerformanceCounter(&next); continue; }

        // Fire action or capture bind on button press
        auto onButton = [&](int b) {
            int listening = g_listeningFor.load();
            if (listening >= 0)
            {
                g_bindings[listening] = { true, b };
                g_listeningFor.store(-1);
                SaveBindings();
            }
            else
            {
                for (int a = 0; a < ACTION_COUNT; ++a)
                    if (g_bindings[a].bound && g_bindings[a].button == b)
                    {
                        if (a == ACTION_NEXT) SpotifyNext();
                        if (a == ACTION_PREV) SpotifyPrevious();
                    }
            }
            };

        if (useGamepad && gamepad)
        {
            SDL_GameControllerUpdate();
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            {
                bool cur = SDL_GameControllerGetButton(gamepad, (SDL_GameControllerButton)b) != 0;
                if (cur && !prevPad[b]) onButton(b);
                prevPad[b] = cur;
            }
            // Triggers (LT = axis 4, RT = axis 5), range 0..32767
            for (int ax = SDL_CONTROLLER_AXIS_TRIGGERLEFT; ax <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT; ++ax)
            {
                bool cur = SDL_GameControllerGetAxis(gamepad, (SDL_GameControllerAxis)ax) > TRIGGER_THRESHOLD;
                if (cur && !prevPadAxis[ax]) onButton(AXIS_BUTTON_OFFSET + ax);
                prevPadAxis[ax] = cur;
            }
        }
        else if (joystick)
        {
            SDL_JoystickUpdate();
            int n = SDL_JoystickNumButtons(joystick);
            if (n > 64) n = 64;
            for (int b = 0; b < n; ++b)
            {
                bool cur = SDL_JoystickGetButton(joystick, b) != 0;
                if (cur && !prevJoy[b]) onButton(b);
                prevJoy[b] = cur;
            }
            // Triggers — DS4 L2=axis3, R2=axis4, rest at -32768, full at 32767
            int numAxes = SDL_JoystickNumAxes(joystick);
            if (numAxes > 16) numAxes = 16;
            for (int ax = 0; ax < numAxes; ++ax)
            {
                bool cur = SDL_JoystickGetAxis(joystick, ax) > TRIGGER_THRESHOLD;
                if (cur && !prevJoyAxis[ax]) onButton(AXIS_BUTTON_OFFSET + ax);
                prevJoyAxis[ax] = cur;
            }
        }

        // Precise 4ms interval — sleep most of it, spin the last 0.5ms
        next.QuadPart += ticksPerPoll;
        QueryPerformanceCounter(&now);
        LONGLONG rem = next.QuadPart - now.QuadPart;
        if (rem > freq.QuadPart / 2000)
            Sleep((DWORD)((rem - freq.QuadPart / 2000) * 1000 / freq.QuadPart));
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
    }

    closeController();
}

// Entry point
int main(int, char**)
{
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
        return 1;
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    SDL_Window* window = SDL_CreateWindow("DS4 Spotify Controller",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 533, 150,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) return 1;

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);
    HWND hwnd = wm.info.win.window;

    HICON hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    if (hIcon) { SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon); SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon); }

    COLORREF captionColor = RGB(18, 18, 18);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)TrayWndProc);
    TrayAdd(hwnd);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.25f;
    ImFontConfig cfg; cfg.SizePixels = 12.5f;
    g_fontStatus = io.Fonts->AddFontDefault(&cfg);
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    LoadBindings();
    std::thread ctrlThread(ControllerThread);

    const float colAction = 10.f, colBound = 200.f, colBtn = 370.f;
    bool done = false;

    while (!done)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT)             done = true;
            else if (e.type == SDL_JOYDEVICEADDED)   g_deviceAdded.store(e.jdevice.which);
            else if (e.type == SDL_JOYDEVICEREMOVED) g_deviceRemoved.store(e.jdevice.which);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        ImGui::PushFont(g_fontStatus);
        bool spotifyUp = FindSpotifyWindow() != nullptr;
        ImGui::TextColored(spotifyUp ? ImVec4{ 0.3f,1.f,0.3f,1.f } : ImVec4{ 1.f,0.4f,0.4f,1.f },
            "Spotify: %s", spotifyUp ? "Running" : "Not detected");
        bool ctrlUp = g_controllerType != ControllerType::Unknown;
        ImGui::TextColored(ctrlUp ? ImVec4{ 0.4f,0.8f,1.f,1.f } : ImVec4{ 0.6f,0.6f,0.6f,1.f },
            "Controller: %s", ctrlUp ? GetControllerTypeName(g_controllerType) : "Not detected");
        ImGui::PopFont();

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::SetCursorPosX(colAction); ImGui::Text("Action");
        ImGui::SameLine(colBound);       ImGui::Text("Bound Button");
        ImGui::Separator(); ImGui::Spacing();

        for (int a = 0; a < ACTION_COUNT; ++a)
        {
            ImGui::PushID(a);
            bool isListening = (g_listeningFor.load() == a);

            ImGui::SetCursorPosX(colAction); ImGui::Text("%s", ActionNames[a]);
            ImGui::SameLine(colBound);
            ImGui::Text("%s", g_bindings[a].bound
                ? GetButtonName(g_controllerType, g_bindings[a].button) : "(unbound)");
            ImGui::SameLine(colBtn);

            if (isListening)
            {
                Uint32 elapsed = SDL_GetTicks() - g_listenStartTime.load();
                if (elapsed >= 3000) g_listeningFor.store(-1);
                Uint32 secs = 3000 > elapsed ? (3000 - elapsed + 999) / 1000 : 0;
                char lbl[32]; snprintf(lbl, sizeof(lbl), "Waiting... (%us)##bind", secs);
                ImGui::PushStyleColor(ImGuiCol_Button, { 0.8f, 0.5f, 0.f, 1.f });
                if (ImGui::Button(lbl, { 150, 0 })) g_listeningFor.store(-1);
                ImGui::PopStyleColor();
            }
            else
            {
                if (ImGui::Button("Bind##bind", { 150, 0 }))
                {
                    g_listeningFor.store(a); g_listenStartTime.store(SDL_GetTicks());
                }
            }

            ImGui::PopID();
            ImGui::Spacing();
        }

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    g_running = false;
    ctrlThread.join();
    TrayRemove();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}