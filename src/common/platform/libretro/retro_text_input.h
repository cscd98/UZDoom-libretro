#pragma once

#include "libretro.h"
#include <array>
#include <vector>

namespace Libretro
{
class TextInput
{
    std::vector<uint32_t> pending;
    std::array<bool, RETROK_LAST> previous{}, translated{}, fallback{};
    bool unkeyedText = false;
    bool capsLock = false;
    bool modifiersReceived = false;

    static unsigned ASCII(unsigned key, bool shift, bool caps)
    {
        if (key >= RETROK_a && key <= RETROK_z)
            return shift != caps ? key - 'a' + 'A' : key;

        if (key < 32 || key > 126)
            return 0;

        if (shift)
        {
            const char plain[] = "1234567890-=[]\\;',./`";
            const char shifted[] = "!@#$%^&*()_+{}|:\"<>?~";

            for (unsigned i = 0; plain[i]; ++i)
                if (key == unsigned(plain[i])) return shifted[i];
        }

        return key;
    }

public:
    void KeyboardEvent(bool down, unsigned key, uint32_t character, uint16_t modifiers)
    {
        if (character || modifiers)
        {
            capsLock = (modifiers & RETROKMOD_CAPSLOCK) != 0;
            modifiersReceived = true;
        }

        if (!down || character < 32 || character > 0xffff)
            return;

        if (key > RETROK_UNKNOWN && key < RETROK_LAST)
        {
            translated[key] = true;
            // A callback arriving one poll late must not duplicate the fallback.
            if (fallback[key])
            {
                fallback[key] = false;
                return;
            }
        }
        else
            unkeyedText = true;

        if (pending.size() < 64)
            pending.push_back(character);
    }

    template<class Allow, class Emit>
    void Poll(const std::array<bool, RETROK_LAST> &keys, bool capture, Allow allow, Emit emit)
    {
        if (!modifiersReceived && keys[RETROK_CAPSLOCK] && !previous[RETROK_CAPSLOCK])
            capsLock = !capsLock;

        if (capture)
        {
            for (auto character : pending)
                emit(character);

            const bool shift = keys[RETROK_LSHIFT] || keys[RETROK_RSHIFT];
            const bool shortcut = keys[RETROK_LCTRL] || keys[RETROK_RCTRL] ||
                keys[RETROK_LALT] || keys[RETROK_RALT] ||
                keys[RETROK_LMETA] || keys[RETROK_RMETA];
            if (!shortcut && !unkeyedText)
            {
                for (unsigned key = 0; key < keys.size(); ++key)
                {
                    if (keys[key] && !previous[key] && !translated[key] && allow(key))
                    {
                        if (auto character = ASCII(key, shift, capsLock))
                        {
                            emit(character);
                            fallback[key] = true;
                        }
                    }
                }
            }
        }

        for (unsigned key = 0; key < keys.size(); ++key)
        {
            if (!keys[key])
                fallback[key] = false;
        }

        previous = keys;
        pending.clear();
        translated.fill(false);
        unkeyedText = false;
        modifiersReceived = false;
    }
};
}
