#include "retro_platform.h"
#include "i_sound.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>

namespace Libretro
{
namespace
{
std::mutex audioMutex;
std::deque<int16_t> pending;
retro_audio_sample_t sample = nullptr;
retro_audio_sample_batch_t batch = nullptr;
bool asynchronous = false, enabled = false;
unsigned remainder = 0;

void Drain()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    if (asynchronous && !enabled)
        return;

    std::array<int16_t, 4096> buffer;
    while (!pending.empty())
    {
        size_t count = std::min(buffer.size(), pending.size());
        std::copy_n(pending.begin(), count, buffer.begin());
        size_t consumed = count / 2;

        if (batch)
            consumed = std::min(consumed, batch(buffer.data(), consumed));
        else if (sample)
            for (size_t i = 0; i < consumed; ++i)
                sample(buffer[2*i], buffer[2*i+1]);
        pending.erase(pending.begin(), pending.begin() + consumed * 2);

        if (consumed * 2 < count)
            break;
    }
}

void SetState(bool state)
{
    std::lock_guard<std::mutex> lock(audioMutex);

    enabled = state;
    if (!state)
        pending.clear();
}

} // anonymous namespace

void SetAudioSample(retro_audio_sample_t callback)
{
    std::lock_guard<std::mutex> lock(audioMutex); sample = callback;
}

void SetAudioBatch(retro_audio_sample_batch_t callback)
{
    std::lock_guard<std::mutex> lock(audioMutex);
    batch = callback;
}

void InitAudio()
{
    retro_audio_callback callback{Drain, SetState};
    asynchronous = Environment && Environment(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &callback);
}

void ResetAudio()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    pending.clear();
    remainder = 0;
}

void RenderAudio()
{
    // Advance the mixer on the engine thread
    const unsigned rate = std::max(1u, unsigned(std::lround(FrameRate)));
    remainder += 48000;
    unsigned frames = remainder / rate;
    remainder %= rate;
    std::array<int16_t, 2744> samples{};

    if (GSnd) GSnd->RenderSamples(samples.data(), frames);
    {
        std::lock_guard<std::mutex> lock(audioMutex);
    
        if (!asynchronous || enabled)
        {
            // Bound latency when a frontend stops consuming without notifying us.
            if (pending.size() + frames * 2 > 96000)
                pending.clear();

            pending.insert(pending.end(), samples.begin(), samples.begin() + frames * 2);
        }
    }

    if (!asynchronous)
        Drain();
}
} // namespace Libretro
