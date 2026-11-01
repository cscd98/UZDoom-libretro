#include "retro_platform.h"
#include "c_cvars.h"
#include "menu.h"
#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>
#include <initializer_list>

namespace Libretro
{
namespace
{
std::deque<std::string> strings;
std::vector<retro_core_option_v2_definition> definitions;
std::map<std::string, std::string> applied;
bool optionsApplied = false;
struct Domain { const char *name, *group; double low, high, step; };

const char *Keep(std::string value)
{
    strings.push_back(std::move(value));
    return strings.back().c_str();
}

#if 0
const Domain domains[] = {
#include "retro_menu_domains.h"
};
#endif

const char *padNames[] = {"b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "l2", "r2", "l3", "r3"};
const unsigned padDefaults[] = {29, 57, 15, 1, 200, 208, 203, 205, PadUse, PadPreviousWeapon, 30, 32, 42, PadFire, 46, 33};

struct Binding { const char *value, *label; unsigned scan; };

const Binding bindings[] = {
    {"attack", "Fire", PadFire},
    {"use", "Use / Open doors", PadUse},
    {"weapprev", "Previous weapon", PadPreviousWeapon},
    {"weapnext", "Next weapon", PadNextWeapon},
    {"none", "Unbound", 0},
    {"ctrl", "Fire (Left Ctrl)", 29},
    {"e", "Use (E)", 18},
    {"space", "Jump (Space)", 57},
    {"shift", "Run (Left Shift)", 42},
    {"c", "Crouch (C)", 46},
    {"tab", "Automap (Tab)", 15},
    {"escape", "Menu (Escape)", 1},
    {"enter", "Enter", 28},
    {"up", "Forward (Up)", 200},
    {"down", "Backward (Down)", 208},
    {"left", "Turn left (Left)", 203},
    {"right", "Turn right (Right)", 205},
    {"a", "Strafe left (A)", 30},
    {"d", "Strafe right (D)", 32},
    {"q", "Q", 16},
    {"f", "F", 33},
    {"1", "Weapon 1", 2},
    {"2", "Weapon 2", 3},
    {"3", "Weapon 3", 4},
    {"4", "Weapon 4", 5},
    {"5", "Weapon 5", 6},
    {"6", "Weapon 6", 7},
    {"7", "Weapon 7", 8},
    {"f6", "Quicksave (F6)", 64},
    {"f9", "Quickload (F9)", 67},
    {"backquote", "Console", 41}
};

std::array<unsigned, 16> pad{};
float hudOffset = 0.3f;
bool laserEnabled = true, autoResolution = true;
unsigned pendingWidth = 0, pendingHeight = 0;
unsigned rejectedWidth = 0, rejectedHeight = 0;

const char *resolutions[] =
{
    "640x480", "800x600", "1024x768", "1280x720", "1280x960",
    "1600x1200", "1920x1080", "1920x1440", "2560x1440", "3840x2160"
};

const char *Choice(const char *key, std::initializer_list<const char *> values, const char *fallback)
{
    retro_variable option{key, nullptr};

    if (Environment && Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) && option.value)
        for (auto value : values) if (!strcmp(value, option.value))
            return value;

    return fallback;
}

retro_core_option_v2_category categories[] = {
    {"graphics", "Graphics", "Output resolution, antialiasing and HUD placement."},
    {"input", "Input", "Mouse sensitivity and keyboard bindings used by the retropad."},
#if 0
    {"engine", "Engine settings", "Standalone console settings. Engine configuration preserves the INI or in-game value."},
#endif
    {nullptr, nullptr, nullptr}
};

void AddValue(retro_core_option_v2_definition& def, std::string value)
{
    for (unsigned i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1; ++i)
    {
        if (!def.values[i].value)
        {
            def.values[i] = {Keep(value), nullptr};

            return;
        }

        if (value == def.values[i].value)
            return;
    }
}

std::string Number(double number)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.8g", number);

    return buffer;
}
}

float VRHUDOffset()
{
    return hudOffset;
}

bool VRLaserEnabled()
{
    return laserEnabled;
}

namespace
{
unsigned recommendedWidth = 0, recommendedHeight = 0;
}

bool AwaitVRResolution()
{
    if (!autoResolution || recommendedWidth != 0)
        return false;

    unsigned status = 0;
    if (!Environment || !Environment(RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, &status))
        return false;

    if (status & RETRO_VIDEO_VIEWS_STATUS_HMD)
        return true;

    // An immersive frontend may need a few frames to start its headset session.
    retro_variable option{"uzdoom_vr", nullptr};

    return !UseVulkan && Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) &&
        option.value && std::strcmp(option.value, "immersive") == 0;
}

void QueueVRResolution(unsigned width, unsigned height, bool requested)
{
    if (!autoResolution)
        return;

    // Recommendations are independent of whether this frame has valid poses.
    // Keep the last size during tracking loss instead of falling back to 640x480.
    if (width && height && width <= 8192 && height <= 8192)
    {
        recommendedWidth = 2 * unsigned(width * 1.25);
        recommendedHeight = unsigned(height * 1.25);
    }
    if (!requested)
    {
        pendingWidth = 640;
        pendingHeight = 480;
    }
    else if (recommendedWidth)
    {
        pendingWidth = recommendedWidth;
        pendingHeight = recommendedHeight;
    }
}

void ApplyVRResolution(bool allowGrowth)
{
    const unsigned nextMaxWidth = std::max(MaxWidth, recommendedWidth);
    const unsigned nextMaxHeight = std::max(MaxHeight, recommendedHeight);

    if (!pendingWidth || !pendingHeight ||
        (pendingWidth == Width && pendingHeight == Height && nextMaxWidth == MaxWidth && nextMaxHeight == MaxHeight))
        return;

    if (pendingWidth == rejectedWidth && pendingHeight == rejectedHeight)
        return;

    if (!allowGrowth && (nextMaxWidth > MaxWidth || nextMaxHeight > MaxHeight))
    {
        rejectedWidth = pendingWidth;
        rejectedHeight = pendingHeight;
        Log(RETRO_LOG_WARN, "Headset render target changed after startup. Reload content to renegotiate its size.\n");
        return;
    }

    const unsigned oldWidth = Width, oldHeight = Height, oldMaxWidth = MaxWidth, oldMaxHeight = MaxHeight;
    Width = pendingWidth;
    Height = pendingHeight;
    MaxWidth = nextMaxWidth;
    MaxHeight = nextMaxHeight;
    retro_system_av_info info{{Width, Height, MaxWidth, MaxHeight, float(Width)/Height}, {FrameRate, 48000.0}};

    if (Environment && Environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info))
    {
        ResizeOutput();

        char message[160];
        std::snprintf(message, sizeof(message), "Automatic render target: %ux%u; headset target: %ux%u per eye.\n",
            Width, Height, recommendedWidth / 2, recommendedHeight);
        Log(RETRO_LOG_INFO, message);
    }
    else
    {
        rejectedWidth = pendingWidth;
        rejectedHeight = pendingHeight;
        Width = oldWidth;
        Height = oldHeight;
        MaxWidth = oldMaxWidth;
        MaxHeight = oldMaxHeight;

        Log(RETRO_LOG_WARN, "Frontend rejected the headset render size; keeping the current resolution.\n");
    }
}

void ConfigureResolution()
{
    Width = 640; Height = 480;
    MaxWidth = 3840; MaxHeight = 2160;
    pendingWidth = pendingHeight = rejectedWidth = rejectedHeight = 0;
    recommendedWidth = recommendedHeight = 0;
    autoResolution = true;

    retro_variable option{"uzdoom_resolution", nullptr};

    if (Environment && Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) && option.value)
    {
        for (auto resolution : resolutions)
        {
            if (!strcmp(resolution, option.value))
            {
                std::sscanf(resolution, "%ux%u", &Width, &Height);
                MaxWidth = std::max(MaxWidth, Width); MaxHeight = std::max(MaxHeight, Height);
                autoResolution = false;

                break;
            }
        }
    }
}

bool VRSticksSwapped()
{
    // Restore the established stick-role preference; it never changes weapon hands.
    return !strcmp(Choice("uzdoom_vr_hands", {"swapped", "standard"}, "swapped"), "swapped");
}

unsigned PadBinding(unsigned button)
{
    return pad.at(button);
}

void RegisterOptions(bool engine)
{
    definitions.clear();
    strings.clear();
    applied.clear();
    optionsApplied = false;

    retro_core_option_v2_definition resolution{};
    resolution.key = "uzdoom_resolution"; resolution.desc = "Output resolution (restart required)";
    resolution.category_key = "graphics"; resolution.default_value = "auto";
    resolution.info = "Automatic in VR, specify otherwise. Manual sizes are total output, shared by both eyes in VR. Reload content after changing. Higher resolutions and supersampling increase GPU and memory use.";
    resolution.values[0] = {"auto", "Automatic (headset recommendation x 1.25)"};

    for (auto value : resolutions)
        AddValue(resolution, value);

    definitions.push_back(resolution);

    auto graphics = [&](const char *key, const char *label, const char *info,
        const char *fallback, std::initializer_list<const char *> values) {
        retro_core_option_v2_definition def{};
        def.key = key; def.desc = label; def.info = info;
        def.category_key = "graphics"; def.default_value = fallback;

        for (auto value : values)
            AddValue(def, value);

        definitions.push_back(def);
    };

    graphics("uzdoom_vr_hands", "VR thumbstick roles", "Swap thumbsticks only. Gun tracking and triggers do not change.",
        "swapped", {"standard", "swapped"});
    definitions.back().category_key = "input";

    graphics("uzdoom_mouse_sensitivity", "Mouse sensitivity", "Gameplay mouse sensitivity for both axes. 2 is the engine default. Menu cursor speed is unchanged.",
        "2", {"0.25", "0.5", "0.75", "1", "1.25", "1.5", "2", "2.5", "3", "4", "5", "6", "8"});
    definitions.back().category_key = "input";

    graphics("uzdoom_anisotropic", "Anisotropic filtering", "Texture filtering at oblique angles. Limited to GPU support.",
        "4", {"1", "2", "4", "8", "16"});
    const char *anisotropicLabels[] = {"1x (Off)", "2x", "4x", "8x", "16x"};

    for (unsigned i = 0; i < 5; ++i)
        definitions.back().values[i].label = anisotropicLabels[i];

    graphics("uzdoom_msaa", "MSAA", "Samples per pixel; 1 disables MSAA. Limited to GPU support. GLES requires multisampled render-to-texture support.",
        "1", {"1", "2", "4", "8"});
    definitions.back().values[0].label = "1x (Off)";

    graphics("uzdoom_supersampling", "Supersampling", "Internal resolution multiplier per axis. 1 is native; 2 renders four times as many pixels and downsamples to the output resolution.",
        "1", {"1", "1.25", "1.5", "1.75", "2"});
    definitions.back().values[0].label = "1x (Native)";

    retro_core_option_v2_definition vr{};
    vr.key = "uzdoom_vr";
    vr.desc = "VR mode";
    vr.category_key = "graphics";
    vr.info = "GL/GLES only. Stereoscopic sends two eye views. Immersive uses headset eye poses and FOV when tracking is available. Unsupported frontends use normal output.";
    vr.default_value = "off";
    vr.values[0] = {"off", "Off"};
    vr.values[1] = {"stereoscopic", "Stereoscopic"};
    vr.values[2] = {"immersive", "Immersive"};
    definitions.push_back(vr);

    retro_core_option_v2_definition laser{};
    laser.key = "uzdoom_vr_laser";
    laser.desc = "VR laser sight";
    laser.category_key = "graphics";
    laser.info = "VR only: show a red aiming dot on the hit surface. The ordinary HUD crosshair is hidden in VR even when the laser is off.";
    laser.default_value = "on";
    laser.values[0] = {"on", "On"};
    laser.values[1] = {"off", "Off"};
    definitions.push_back(laser);

    graphics("uzdoom_vr_hud_offset", "Immersive HUD vertical offset", "Fraction of HUD height to move downward. 0 centres the HUD plane; 0.3 places it lower. Does not move the weapon.",
        "0.3", {"0", "0.1", "0.2", "0.3", "0.4"});

    for (unsigned i = 0; i < 16; ++i)
    {
        retro_core_option_v2_definition def{};
        def.key = Keep(std::string("uzdoom_pad_") + padNames[i]);
        def.desc = Keep(std::string("Retropad ") + padNames[i]);
        def.category_key = "input";

        for (unsigned n = 0; n < std::size(bindings); ++n)
        {
            def.values[n] = {bindings[n].value, bindings[n].label};
            if (bindings[n].scan == padDefaults[i])
                def.default_value = bindings[n].value;
        }

        pad[i] = padDefaults[i];
        definitions.push_back(def);
    }

#if 0
    if (engine)
    {
        CVarMap::Iterator it(cvarMap);
        CVarMap::Pair *pair;
        std::vector<FBaseCVar *> vars;

        while (it.NextPair(pair))
            vars.push_back(pair->Value);

        std::sort(vars.begin(), vars.end(), [](auto a, auto b)
        {
            return strcmp(a->GetName(), b->GetName()) < 0;
        });

        for (auto var : vars)
        {
            if (!strcmp(var->GetName(), "gl_multisample") || !strcmp(var->GetName(), "vid_scalefactor")
                || !strcmp(var->GetName(), "gl_texture_filter_anisotropic")
                || !strcmp(var->GetName(), "m_sensitivity_x")
                || !strcmp(var->GetName(), "m_sensitivity_y"))
            {
                continue;
            }
            if (var->GetFlags() & (CVAR_NOSET | CVAR_HIDDEN))
                continue;

            retro_core_option_v2_definition def{};
            def.key = Keep(std::string("uzdoom_cvar_") + var->GetName());
            def.desc = Keep(var->GetName()); def.category_key = "engine";
            def.info = Keep(std::string(var->GetDescription().GetChars()) +
                " Engine configuration uses the INI/in-game setting. Latched settings apply on restart.");
            def.default_value = "engine";
            def.values[0] = {"engine", "Engine configuration"};

            AddValue(def, var->GetGenericRepDefault(CVAR_String).String);
            AddValue(def, var->GetGenericRep(CVAR_String).String);

            if (var->GetRealType() == CVAR_Bool || var->GetRealType() == CVAR_Flag)
            {
                AddValue(def, "false");
                AddValue(def, "true");
            }

            for (auto& domain : domains)
            {
                if (strcmp(domain.name, var->GetName()))
                    continue;

                if (domain.group)
                {
                    auto values = OptionValues.CheckKey(FName(domain.group));

                    if (values) for (auto& value : (*values)->mValues)
                        AddValue(def, value.TextValue.IsEmpty() ? Number(value.Value) : value.TextValue.GetChars());
                }
                else if (domain.step > 0 && domain.high >= domain.low)
                {
                    // v2 has a fixed 128-value limit. Retain both range endpoints.
                    unsigned steps = unsigned((domain.high - domain.low) / domain.step + 0.5);
                    unsigned stride = std::max(1u, (steps + 121) / 122);

                    for (unsigned n = 0; n <= steps; n += stride)
                        AddValue(def, Number(domain.low + n * domain.step));

                    AddValue(def, Number(domain.high));
                }
            }

            definitions.push_back(def);
        }
    }
#endif

    definitions.push_back({});
    retro_core_options_v2 options{categories, definitions.data()};
    if (Environment)
        Environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}

void ApplyOptions()
{
    if (!Environment)
        return;

    bool changed = false;
    if (optionsApplied && Environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && !changed)
        return;

    optionsApplied = true;

    for (unsigned i = 0; i < 16; ++i)
    {
        std::string key = std::string("uzdoom_pad_") + padNames[i];
        retro_variable value{key.c_str(), nullptr};

        if (Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &value) && value.value)
            for (auto& binding : bindings) if (!strcmp(binding.value, value.value)) pad[i] = binding.scan;
    }

    for (auto& def : definitions)
    {
        if (!def.key || strncmp(def.key, "uzdoom_cvar_", 12))
            continue;

        retro_variable value{def.key, nullptr};
        if (!Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &value) || !value.value)
            continue;

        auto& last = applied[def.key];
        if (last == value.value)
            continue;

        last = value.value;
        if (last == "engine")
            continue;

        if (auto var = FindCVar(def.key + 12, nullptr))
            var->CmdSet(value.value);
    }

    auto set = [&](const char *name, const char *value) {
        auto &last = applied[name];
        if (last != value)
            if (auto var = FindCVar(name, nullptr))
            {
                var->CmdSet(value); last = value;
            }
    };

    const char *mouseSensitivity = Choice("uzdoom_mouse_sensitivity",
        {"0.25", "0.5", "0.75", "1", "1.25", "1.5", "2", "2.5", "3", "4", "5", "6", "8"}, "2");

    set("m_sensitivity_x", mouseSensitivity);
    set("m_sensitivity_y", mouseSensitivity);
    set("gl_texture_filter_anisotropic", Choice("uzdoom_anisotropic", {"1", "2", "4", "8", "16"}, "4"));
    laserEnabled = !strcmp(Choice("uzdoom_vr_laser", {"on", "off"}, "on"), "on");
    set("gl_multisample", Choice("uzdoom_msaa", {"1", "2", "4", "8"}, "1"));

    const char *scale = Choice("uzdoom_supersampling", {"1", "1.25", "1.5", "1.75", "2"}, "1");

    if (applied["vid_scalefactor"] != scale)
        if (auto mode = FindCVar("vid_scalemode", nullptr))
            mode->CmdSet("0");

    set("vid_scalefactor", scale);

    hudOffset = std::strtof(Choice("uzdoom_vr_hud_offset", {"0", "0.1", "0.2", "0.3", "0.4"}, "0.3"), nullptr);
}
} // anonymous namespace
