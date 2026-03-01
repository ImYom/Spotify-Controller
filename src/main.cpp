#define SDL_MAIN_HANDLED
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <tlhelp32.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

// Spotify helpers
static HWND FindSpotifyWindow()
{
    DWORD spotifyPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do {
                if (_wcsicmp(pe.szExeFile, L"Spotify.exe") == 0)
                {
                    spotifyPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (spotifyPid == 0) return nullptr;

    struct FindData { DWORD pid; HWND hwnd; };
    FindData fd{ spotifyPid, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* fd = reinterpret_cast<FindData*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == fd->pid && IsWindowVisible(hwnd))
        {
            wchar_t cls[256]{};
            GetClassNameW(hwnd, cls, 256);
            if (wcsstr(cls, L"Chrome_WidgetWin") != nullptr)
            {
                fd->hwnd = hwnd;
                return FALSE;
            }
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&fd));

    return fd.hwnd;
}

static void SendSpotifyCommand(LPARAM command)
{
    HWND hw = FindSpotifyWindow();
    if (hw)
        SendMessage(hw, WM_APPCOMMAND, (WPARAM)hw, command);
}

static void SpotifyNext() { SendSpotifyCommand(MAKELPARAM(0, FAPPCOMMAND_KEY | APPCOMMAND_MEDIA_NEXTTRACK)); }
static void SpotifyPrevious() { SendSpotifyCommand(MAKELPARAM(0, FAPPCOMMAND_KEY | APPCOMMAND_MEDIA_PREVIOUSTRACK)); }

enum ActionID { ACTION_NEXT = 0, ACTION_PREV = 1, ACTION_COUNT = 2 };

static const char* ActionNames[ACTION_COUNT] = { "Next Track", "Previous Track" };

struct Binding
{
    bool bound = false;
    int  button = -1;
};

static Binding          g_bindings[ACTION_COUNT];
static std::atomic<int> g_listeningFor{ -1 };

// Font for the Spotify status line (loaded at a larger size)
static ImFont* g_fontStatus = nullptr;

// -----------------------------------------------------------------------
// Controller thread
// Uses raw SDL_Joystick (not SDL_GameController) + SDL_JoystickUpdate()
// which is safe to call from a background thread and works when the app
// is not in focus, provided SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1".
// -----------------------------------------------------------------------

static std::atomic<bool> g_running{ true };

static void ControllerThread()
{
    // Set lowest thread priority to make sure the game always wins
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    SDL_InitSubSystem(SDL_INIT_JOYSTICK);

    SDL_Joystick* joystick = nullptr;

    auto tryOpen = [&]() {
        if (joystick) return;
        if (SDL_NumJoysticks() > 0)
            joystick = SDL_JoystickOpen(0);
        };

    tryOpen();

    bool prevState[32] = {};

    // Poll at 250Hz (4ms) to matches DS4 USB HID report rate.
    // SDL_Delay is imprecise so we use a high-res spin-wait for the interval, but only after sleeping most of the 4ms to avoid burning a full CPU core.

    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG ticksPerPoll = freq.QuadPart / 250; // 4ms in ticks

    while (g_running)
    {
        tryOpen();

        if (!joystick)
        {
            Sleep(500);
            QueryPerformanceCounter(&next);
            continue;
        }

        SDL_JoystickUpdate();

        if (!SDL_JoystickGetAttached(joystick))
        {
            SDL_JoystickClose(joystick);
            joystick = nullptr;
            Sleep(500);
            QueryPerformanceCounter(&next);
            continue;
        }

        int numButtons = SDL_JoystickNumButtons(joystick);
        if (numButtons > 32) numButtons = 32;

        for (int b = 0; b < numButtons; ++b)
        {
            bool cur = SDL_JoystickGetButton(joystick, b) != 0;
            bool prev = prevState[b];

            if (cur && !prev)
            {
                int listening = g_listeningFor.load();
                if (listening >= 0)
                {
                    g_bindings[listening].button = b;
                    g_bindings[listening].bound = true;
                    g_listeningFor.store(-1);
                }
                else
                {
                    for (int a = 0; a < ACTION_COUNT; ++a)
                    {
                        if (g_bindings[a].bound && g_bindings[a].button == b)
                        {
                            if (a == ACTION_NEXT) SpotifyNext();
                            if (a == ACTION_PREV) SpotifyPrevious();
                        }
                    }
                }
            }
            prevState[b] = cur;
        }

        // Precise 4ms interval: sleep most of it, spin-wait the last 0.5ms
        next.QuadPart += ticksPerPoll;
        QueryPerformanceCounter(&now);
        LONGLONG remaining = next.QuadPart - now.QuadPart;
        LONGLONG halfMs = freq.QuadPart / 2000;
        if (remaining > halfMs)
            Sleep((DWORD)((remaining - halfMs) * 1000 / freq.QuadPart));
        // Spin for the final sub-ms to land precisely on the interval
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
    }

    if (joystick) SDL_JoystickClose(joystick);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

// DS4 Button Lables

static const char* DS4ButtonName(int b)
{
    static const char* names[] = {
        "Cross", "Circle", "Square", "Triangle",
        "Share", "PS", "Options",
        "L3", "R3", "L1", "R1",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
        "Touchpad",
    };
    if (b >= 0 && b < (int)(sizeof(names) / sizeof(names[0])))
        return names[b];
    return "Unknown";
}

int main(int, char**)
{
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return 1;

    SDL_Window* window = SDL_CreateWindow(
        "DS4 Spotify Controller",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        533, 133,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) return 1;

    // Set window icon from the embedded resource (resource ID 1)
    {
        HICON hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
        if (hIcon)
        {
            HWND hwnd2{};
            SDL_SysWMinfo wm{};
            SDL_VERSION(&wm.version);
            SDL_GetWindowWMInfo(window, &wm);
            hwnd2 = wm.info.win.window;
            SendMessage(hwnd2, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwnd2, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }

    // Set title bar color (Windows 11 only; no-op on Windows 10)
    {
        SDL_SysWMinfo wmInfo{};
        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(window, &wmInfo);
        HWND hwnd = wmInfo.info.win.window;
        // RGB(r, g, b) — currently matches the dark background, change to taste
        COLORREF captionColor = RGB(18, 18, 18);
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.FontGlobalScale = 1.25f;

        // Load the built-in font a second time at a larger size for the status line.
        // AddFontDefault() always uses 13px; to get a different size we use
        // AddFontFromMemoryCompressedBase85TTF with the proggy clean data.
        ImFontConfig cfg;
        cfg.SizePixels = 12.5f;
        g_fontStatus = io.Fonts->AddFontDefault(&cfg);
    }
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::thread ctrlThread(ControllerThread);

    // Column x-offsets (pixels, relative to window content area)
    const float colAction = 10.f;
    const float colBound = 200.f;
    const float colBtn = 370.f;

    bool done = false;
    while (!done)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) done = true;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        // Spotify status
        bool spotifyRunning = FindSpotifyWindow() != nullptr;
        ImGui::PushFont(g_fontStatus);
        if (spotifyRunning)
            ImGui::TextColored({ 0.3f, 1.f, 0.3f, 1.f }, "Spotify: Running");
        else
            ImGui::TextColored({ 1.f, 0.4f, 0.4f, 1.f }, "Spotify: Not detected");
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Table header
        ImGui::SetCursorPosX(colAction);
        ImGui::Text("Action");
        ImGui::SameLine(colBound);
        ImGui::Text("Bound Button");
        ImGui::Separator();
        ImGui::Spacing();

        // Binding rows
        for (int a = 0; a < ACTION_COUNT; ++a)
        {
            ImGui::PushID(a);

            bool isListening = (g_listeningFor.load() == a);

            ImGui::SetCursorPosX(colAction);
            ImGui::Text("%s", ActionNames[a]);
            ImGui::SameLine(colBound);
            ImGui::Text("%s", g_bindings[a].bound
                ? DS4ButtonName(g_bindings[a].button)
                : "(unbound)");
            ImGui::SameLine(colBtn);

            if (isListening)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, { 0.8f, 0.5f, 0.f, 1.f });
                if (ImGui::Button("Press a button...##bind", { 150, 0 }))
                    g_listeningFor.store(-1);
                ImGui::PopStyleColor();
            }
            else
            {
                if (ImGui::Button("Bind##bind", { 150, 0 }))
                    g_listeningFor.store(a);
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

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}