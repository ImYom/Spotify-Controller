#include "controllers.h"
#include <SDL.h>
#include <cctype>
#include <algorithm>

static const char* s_ds4Names[] = {
    "Cross", "Circle", "Square", "Triangle",
    "Share", "PS", "Options", "L3", "R3", "L1", "R1",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "Touchpad",
};

static const char* s_ds5Names[] = {
    "Cross", "Circle", "Square", "Triangle",
    "Create", "PS", "Options", "L3", "R3", "L1", "R1",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "Touchpad", "Mute",
};

// Matches SDL_GameControllerButton enum order (XInput path)
static const char* s_xboxNames[] = {
    "A", "B", "X", "Y",
    "View", "Xbox", "Menu", "L3", "R3", "LB", "RB",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "Share",
};

static std::string ToLower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

ControllerType DetectControllerType(const char* sdlName)
{
    if (!sdlName) return ControllerType::Unknown;
    std::string n = ToLower(sdlName);

    if (n.find("dualsense") != std::string::npos)
        return ControllerType::DS5;
    if (n.find("dualshock") != std::string::npos ||
        n.find("wireless controller") != std::string::npos ||
        n.find("ps4 controller") != std::string::npos ||
        n.find("ps4") != std::string::npos)
        return ControllerType::DS4;
    if (n.find("xbox") != std::string::npos ||
        n.find("xinput") != std::string::npos ||
        n.find("x-input") != std::string::npos ||
        n.find("for windows") != std::string::npos)
        return ControllerType::XboxOne;

    return ControllerType::Unknown;
}

const char* GetButtonName(ControllerType type, int button)
{
    if (button < 0) return "Unknown";

    // Virtual trigger axis buttons (offset 1000+)
    if (button >= 1000)
    {
        int ax = button - 1000;
        switch (type)
        {
        case ControllerType::XboxOne:
            if (ax == SDL_CONTROLLER_AXIS_TRIGGERLEFT)  return "LT";
            if (ax == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return "RT";
            break;
        case ControllerType::DS4:
        case ControllerType::DS5:
            if (ax == 4) return "L2";
            if (ax == 5) return "R2";
            break;
        default: break;
        }
        static char axfallback[16];
        snprintf(axfallback, sizeof(axfallback), "Axis %d", ax);
        return axfallback;
    }

    switch (type)
    {
    case ControllerType::DS4:
        if (button < (int)(sizeof(s_ds4Names) / sizeof(*s_ds4Names)))  return s_ds4Names[button];
        break;
    case ControllerType::DS5:
        if (button < (int)(sizeof(s_ds5Names) / sizeof(*s_ds5Names)))  return s_ds5Names[button];
        break;
    case ControllerType::XboxOne:
        if (button < (int)(sizeof(s_xboxNames) / sizeof(*s_xboxNames))) return s_xboxNames[button];
        break;
    default: break;
    }

    static char fallback[16];
    snprintf(fallback, sizeof(fallback), "Button %d", button);
    return fallback;
}

const char* GetControllerTypeName(ControllerType type)
{
    switch (type)
    {
    case ControllerType::DS4:     return "DualShock 4";
    case ControllerType::DS5:     return "DualSense";
    case ControllerType::XboxOne: return "Xbox";
    default:                      return "Unknown Controller";
    }
}