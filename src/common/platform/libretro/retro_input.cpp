#include "retro_platform.h"
#include "retro_keys.h"
#include "retro_text_input.h"
#include "retro_vr.h"
#include "retro_vr_controls.h"
#include "c_dispatch.h"
#include "d_eventbase.h"
#include "i_interface.h"
#include "m_joy.h"
#include "c_cvars.h"
#include "c_buttons.h"
#include "keydef.h"
#include "menustate.h"
#include "menu.h"
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include "i_time.h"

bool GUICapture = false;
CVAR(Bool, use_mouse, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

namespace
{
std::array<bool, RETROK_LAST> keys{};
std::array<bool, 256> scanKeys{};
bool padEnabled = true;
Libretro::VRStickState vrSticks;
double pendingTurn = 0, smoothTurnRate = 0;
Libretro::VRPressEdge calibrationButton;
Libretro::VRPressEdge menuButton;
Libretro::VRPressEdge keyboardBack;
std::array<bool, 256> blockedKeys{};
std::array<bool, 13> vrButtons{};
std::array<bool, 4> padActions{};

void StandardPadCommands(bool active)
{
    std::array<bool, 4> down{};
    if (active)
    {
        for (unsigned button = 0; button < 16; ++button)
        {
            const unsigned action = Libretro::PadBinding(button);
            if (action >= Libretro::PadFire && action <= Libretro::PadUse)
            {
                down[action - Libretro::PadFire] |= Libretro::InputState(0,
                    RETRO_DEVICE_JOYPAD, 0, button) != 0;
            }
        }
    }
    const char *commands[] = {"+attack", "weapprev", "weapnext", "+use"};
    for (unsigned action = 0; action < down.size(); ++action)
    {
        if (down[action] && !padActions[action])
        {
            AddCommandString(commands[action]);
        }
        else if ((action == 0 || action == 3) && !down[action] && padActions[action])
        {
            AddCommandString(action == 0 ? "-attack" : "-use");
        }
    }
    padActions = down;
}

void VRCommands(bool active)
{
    auto pressed = [&](unsigned id) {
        return active && Libretro::InputState(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    };
    const bool grip = pressed(RETRO_DEVICE_ID_JOYPAD_R);
    const bool calibrating = pressed(RETRO_DEVICE_ID_JOYPAD_L3) && pressed(RETRO_DEVICE_ID_JOYPAD_R3);

    if (calibrationButton.Press(calibrating))
    {
        Libretro::CalibrateVRAim();
    }

    const std::array<bool, 13> down{
        pressed(RETRO_DEVICE_ID_JOYPAD_R2) && !grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_R2) && grip,
        (pressed(RETRO_DEVICE_ID_JOYPAD_B) || (pressed(RETRO_DEVICE_ID_JOYPAD_L3) && !calibrating)) && !grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_A) && !grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_B) && grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_L) && !grip,
        (pressed(RETRO_DEVICE_ID_JOYPAD_X) && !grip) || (pressed(RETRO_DEVICE_ID_JOYPAD_R3) && grip && !calibrating),
        pressed(RETRO_DEVICE_ID_JOYPAD_R3) && !grip && !calibrating,
        false, // Escape/Start has one edge detector shared with the menu.
        pressed(RETRO_DEVICE_ID_JOYPAD_Y) && grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_X) && grip,
        pressed(RETRO_DEVICE_ID_JOYPAD_L3) && grip && !calibrating,
        pressed(RETRO_DEVICE_ID_JOYPAD_L) && grip
    };

    const char *press[] = {"+attack", "+altattack", "+use", "+jump", "+crouch", "+speed", "togglemap", "invuse", "menu_main", "+movedown", "+moveup", "land", "togglemap"};
    const char *release[] = {"-attack", "-altattack", "-use", "-jump", "-crouch", "-speed", nullptr, nullptr, nullptr, "-movedown", "-moveup", nullptr, nullptr};

    for (unsigned i = 0; i < down.size(); ++i)
    {
        if (down[i] != vrButtons[i])
        {
            const char *command = down[i] ? press[i] : release[i];
            if (command)
            {
                AddCommandString(command);
            }
        }
    }

    vrButtons = down;

    if (!active)
    {
        vrSticks = {};
        pendingTurn = 0;
        smoothTurnRate = 0;
        return;
    }

    const float x = Libretro::InputState(0, RETRO_DEVICE_ANALOG,
        Libretro::VRPadStick(RETRO_DEVICE_INDEX_ANALOG_RIGHT,
        Libretro::VRSticksSwapped()), RETRO_DEVICE_ID_ANALOG_X)/32768.0f;
    const float y = Libretro::InputState(0, RETRO_DEVICE_ANALOG,
        Libretro::VRPadStick(RETRO_DEVICE_INDEX_ANALOG_RIGHT,
        Libretro::VRSticksSwapped()), RETRO_DEVICE_ID_ANALOG_Y)/32768.0f;
    const bool smooth = Libretro::VRSmoothTurn();
    const auto actions = vrSticks.Poll(x, y, grip, !smooth);
    pendingTurn += actions.turn * double(Libretro::VRTurnAngle());

    // A rate in degrees per second, applied once per game tic so the number of frames per tic cannot vary the turn.
    smoothTurnRate = 0;
    if (smooth && actions.smooth != 0.0f)
    {
        const float magnitude = std::max(0.0f, (std::abs(actions.smooth) - 0.15f) / 0.85f);
        smoothTurnRate = -std::copysign(magnitude, actions.smooth) * 2.0 * Libretro::VRTurnAngle();
    }

    const int weapon = actions.weapon;
    if (weapon)
        AddCommandString(weapon < 0 ? "weapprev" : "weapnext");
}

std::array<bool, 3> mouse{};
Libretro::TextInput textInput;
int mouseX = 320, mouseY = 240;

void Key(unsigned code, bool down, unsigned character = 0)
{
    if (!code)
        return;

    event_t event{};
    event.type = down ? EV_KeyDown : EV_KeyUp;
    event.data1 = code;
    event.data2 = character;

    if (GUICapture && code < 256)
    {
        event.type = EV_GUI_Event;
        event.subtype = down ? EV_GUI_KeyDown : EV_GUI_KeyUp;
        unsigned gui = character;
    
        switch (code)
        {
        case KEY_ESCAPE:
            gui = GK_ESCAPE;
            break;
        case KEY_BACKSPACE:
            gui = GK_BACKSPACE;
            break;
        case KEY_TAB:
            gui = GK_TAB;
            break;
        case KEY_ENTER:
            gui = GK_RETURN;
            break;
        case KEY_HOME:
            gui = GK_HOME;
            break;
        case KEY_UPARROW:
            gui = GK_UP;
            break;
        case KEY_PGUP:
            gui = GK_PGUP;
            break;
        case KEY_LEFTARROW:
            gui = GK_LEFT;
            break;
        case KEY_RIGHTARROW:
            gui = GK_RIGHT;
            break;
        case KEY_END:
            gui = GK_END;
            break;
        case KEY_DOWNARROW:
            gui = GK_DOWN;
            break;
        case KEY_PGDN:
            gui = GK_PGDN;
            break;
        case KEY_DEL:
            gui = GK_DEL;
            break;
        default:
            if (code >= KEY_F1 && code <= KEY_F10)
            {
                gui = GK_F1 + code - KEY_F1;
            }
            break;
        }
    
        if (!gui)
            return;
    
        event.data1 = gui >= 'a' && gui <= 'z' ? gui - 'a' + 'A' : gui;
        event.data3 = ((keys[RETROK_LSHIFT] || keys[RETROK_RSHIFT]) ? GKM_SHIFT : 0) |
            ((keys[RETROK_LCTRL] || keys[RETROK_RCTRL]) ? GKM_CTRL : 0) |
            ((keys[RETROK_LALT] || keys[RETROK_RALT]) ? GKM_ALT : 0);
    }

    D_PostEvent(&event);
}

}

namespace Libretro
{
void ResetInput()
{
    menuButton = {};
    keyboardBack = {};
    blockedKeys.fill(false);
    calibrationButton = {};
    vrButtons.fill(false);
    padActions.fill(false);
    vrSticks = {};
    pendingTurn = 0;
    smoothTurnRate = 0;
    keys.fill(false); scanKeys.fill(false); mouse.fill(false);
    textInput = {}; GUICapture = false; mouseX = Width / 2; mouseY = Height / 2;
}

void KeyboardEvent(bool down, unsigned key, uint32_t character, uint16_t modifiers)
{
    textInput.KeyboardEvent(down, key, character, modifiers);
}

void SetController(unsigned device)
{
    padEnabled = device == RETRO_DEVICE_JOYPAD || device == RETRO_DEVICE_ANALOG;
}

double ConsumeVRTurn()
{
    const double turn = pendingTurn + smoothTurnRate / GameTicRate;
    pendingTurn = 0;
    return turn;
}

double VRSmoothTurnRate()
{
    return smoothTurnRate;
}

void PollInput()
{
    if (!InputState)
        return;

    bool capture = sysCallbacks.WantGuiCapture && sysCallbacks.WantGuiCapture();

    if (capture != GUICapture)
    {
        if (capture)
        {
            buttonMap.ResetButtonStates();
        }
        for (unsigned scan = 0; scan < scanKeys.size(); ++scan)
        {
            blockedKeys[scan] = blockedKeys[scan] || scanKeys[scan];
        }
        scanKeys.fill(false);
        GUICapture = capture;
    }

    std::array<bool, 256> next{};
    std::array<unsigned, 256> characters{};
    const bool vrControls = padEnabled && VRControlsActive() && !GUICapture;
    if (VRControlsActive())
    {
        StandardPadCommands(false);
    }
    VRCommands(vrControls);
    Libretro::PollKeys(InputState, PadBinding, padEnabled && !vrControls, GUICapture,
        keys, next, characters);
    const bool keyboardActive = std::any_of(keys.begin(), keys.end(), [](bool down) { return down; });
    StandardPadCommands(padEnabled && !VRControlsActive() && !GUICapture && !keyboardActive);

    const bool backPressed = keyboardBack.Press(keys[RETROK_z]);

    if (GUICapture && menuactive != MENU_Off)
    {
        // Text fields need literal X/Z, including when opened with a controller.
        const bool enteringText = CurrentMenu && CurrentMenu->IsKindOf("TextEnterMenu");
        if (!enteringText)
        {
            next[KEY_ENTER] = next[KEY_ENTER] || keys[RETROK_x];
            if (backPressed)
            {
                Key(KEY_ESCAPE, true, RETROK_ESCAPE);
                Key(KEY_ESCAPE, false, RETROK_ESCAPE);
            }
            next[ScanCode(RETROK_x)] = false;
            next[ScanCode(RETROK_z)] = false;
        }

        if (padEnabled && VRControlsActive())
        {
            const unsigned stick = VRPadStick(RETRO_DEVICE_INDEX_ANALOG_LEFT, VRSticksSwapped());
            const float x = InputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X)/32768.0f;
            const float y = InputState(0, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y)/32768.0f;
            if (std::abs(y) >= std::abs(x))
            {
                next[KEY_UPARROW] = next[KEY_UPARROW] || y < -0.5f;
                next[KEY_DOWNARROW] = next[KEY_DOWNARROW] || y > 0.5f;
            }
            else
            {
                next[KEY_LEFTARROW] = next[KEY_LEFTARROW] || x < -0.5f;
                next[KEY_RIGHTARROW] = next[KEY_RIGHTARROW] || x > 0.5f;
            }
        }
    }

    // Keep Escape/Start latched across GUI capture transitions. A held button
    // must never become a fresh key-down just because it opened the menu.
    // Like PollKeys, ignore RetroPad input while a keyboard key is held:
    // RetroArch maps Enter to Start, which would cancel text entry with Escape.
    const bool menuDown = next[KEY_ESCAPE] || (padEnabled && !keyboardActive &&
        InputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START));

    if (menuButton.Changed(menuDown))
    {
        Key(KEY_ESCAPE, menuDown, RETROK_ESCAPE);
    }

    next[KEY_ESCAPE] = false;
    scanKeys[KEY_ESCAPE] = false;

    for (unsigned scan = 2; scan < next.size(); ++scan)
    {
        if (!next[scan])
        {
            blockedKeys[scan] = false;
        }
        if (blockedKeys[scan])
        {
            next[scan] = false;
        }
    }

    textInput.Poll(keys, GUICapture,
        [&](unsigned key) { const auto scan = ScanCode(key); return scan && next[scan]; },
        [](uint32_t character) {
            event_t event{};
            event.type = EV_GUI_Event;
            event.subtype = EV_GUI_Char;
            event.data1 = static_cast<int16_t>(character);
            D_PostEvent(&event);
        });

    for (unsigned scan = 1; scan < next.size(); ++scan)
        if (next[scan] != scanKeys[scan])
            Key(scan, next[scan], characters[scan]);

    scanKeys = next;
    const int dx = use_mouse ? InputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) : 0;
    const int dy = use_mouse ? InputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y) : 0;

    mouseX = std::clamp(mouseX + dx, 0, static_cast<int>(Width) - 1);
    mouseY = std::clamp(mouseY + dy, 0, static_cast<int>(Height) - 1);

    if (GUICapture && (dx || dy))
    {
        event_t event{};
        event.type = EV_GUI_Event; event.subtype = EV_GUI_MouseMove;
        event.data1 = mouseX; event.data2 = mouseY;

        D_PostEvent(&event);
    }

    static const unsigned ids[] = {RETRO_DEVICE_ID_MOUSE_LEFT, RETRO_DEVICE_ID_MOUSE_RIGHT, RETRO_DEVICE_ID_MOUSE_MIDDLE};
    static const unsigned guiButtons[] = {EV_GUI_LButtonDown, EV_GUI_RButtonDown, EV_GUI_MButtonDown};

    for (unsigned i = 0; i < mouse.size(); ++i)
    {
        bool down = use_mouse && InputState(0, RETRO_DEVICE_MOUSE, 0, ids[i]) != 0;

        if (down != mouse[i])
        {
            if (GUICapture)
            {
                event_t event{};
                event.type = EV_GUI_Event; event.subtype = guiButtons[i] + (down ? 0 : 1);
                event.data1 = mouseX; event.data2 = mouseY;
                D_PostEvent(&event);
            }
            else Key(KEY_MOUSE1 + i, down);
        }

        mouse[i] = down;
    }

    for (unsigned i = 0; i < 2; ++i)
    {
        if (!use_mouse || !InputState(0, RETRO_DEVICE_MOUSE, 0,
            i ? RETRO_DEVICE_ID_MOUSE_WHEELDOWN : RETRO_DEVICE_ID_MOUSE_WHEELUP))
            continue;
        
        if (GUICapture)
        {
            event_t event{};
            event.type = EV_GUI_Event; event.subtype = i ? EV_GUI_WheelDown : EV_GUI_WheelUp;
            D_PostEvent(&event);
        }
        else
        {
            unsigned key = i ? KEY_MWHEELDOWN : KEY_MWHEELUP;
            Key(key, true); Key(key, false);
        }
    }

    if (!GUICapture)
        PostMouseMove(dx, dy);
}
}

void I_StartTic() {}
void I_StartFrame() {}
void I_ShutdownInput()
{
    Libretro::ResetInput();
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks)
{
    sticks.Clear();
}

IJoystickConfig *I_UpdateDeviceList()
{
    return nullptr;
}

void I_GetAxes(float axes[NUM_AXIS_CODES])
{
    std::fill(axes, axes + NUM_AXIS_CODES, 0.0f);

    if (!padEnabled || !Libretro::InputState || GUICapture || Libretro::VRControlsActive())
    {
        return;
    }

    for (unsigned stick = 0; stick < 2; ++stick)
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            float value = Libretro::InputState(0, RETRO_DEVICE_ANALOG, stick, axis) / 32768.0f;
            float magnitude = std::max(0.0f, (std::abs(value) - 0.15f) / 0.85f);
            unsigned index = AXIS_CODE_PAD_LTHUMB_RIGHT + stick * 4 + axis * 2;
            axes[index + (value < 0 ? 1 : 0)] = magnitude;
        }
}
void I_Rumble(double high, double low, double, double)
{
    if (!Libretro::Environment)
        return;

    retro_rumble_interface rumble{};

    if (Libretro::Environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && rumble.set_rumble_state)
    {
        rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, std::clamp(low, 0.0, 1.0) * 65535);
        rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, std::clamp(high, 0.0, 1.0) * 65535);
    }
}

void I_GetEvent() {}
void I_SetMouseCapture() {}
void I_ReleaseMouseCapture() {}
