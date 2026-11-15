#include "retro_platform.h"
#include "i_sound.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace Libretro
{
unsigned AudioSampleRate = 48000;
namespace
{
std::mutex audioMutex;
std::deque<int16_t> pending;
retro_audio_sample_t sample = nullptr;
retro_audio_sample_batch_t batch = nullptr;
SoundRenderer *audioRenderer = nullptr;
bool asynchronous = false, enabled = false;
double remainder = 0;
unsigned audioGeneration = 0;

void Drain()
{
    std::unique_lock<std::mutex> lock(audioMutex);
    if (asynchronous && !enabled)
        return;

    std::array<int16_t, 4096> buffer{};
    if (asynchronous && pending.empty())
    {
        // Generate audio on demand, independently of slow video frames.
        const size_t frames = std::min(size_t(AudioSampleRate / 100), buffer.size() / 2);
        if (audioRenderer)
            audioRenderer->RenderSamples(buffer.data(), unsigned(frames));
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + frames * 2);
    }
    const unsigned generation = audioGeneration;
    const auto submitBatch = batch;
    const auto submitSample = sample;
    std::deque<int16_t> output;
    output.swap(pending);
    // Frontend submission may block during shutdown. Never hold the mutex
    // needed to disable audio or detach the renderer across frontend calls.
    lock.unlock();
    while (!output.empty())
    {
        size_t count = std::min(buffer.size(), output.size());
        std::copy_n(output.begin(), count, buffer.begin());
        size_t consumed = count / 2;

        if (submitBatch)
            consumed = std::min(consumed, submitBatch(buffer.data(), consumed));
        else if (submitSample)
            for (size_t i = 0; i < consumed; ++i)
                submitSample(buffer[2*i], buffer[2*i+1]);
        output.erase(output.begin(), output.begin() + consumed * 2);

        if (consumed * 2 < count)
            break;
    }
    lock.lock();
    // A reset or state change invalidates samples already in flight.
    if (generation == audioGeneration)
        pending.insert(pending.begin(), output.begin(), output.end());
}

void SetState(bool state)
{
    std::lock_guard<std::mutex> lock(audioMutex);
    ++audioGeneration;
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
    unsigned targetRate = 0;
    AudioSampleRate = 48000;
    if (Environment && Environment(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &targetRate) &&
        targetRate >= 8000 && targetRate <= 384000)
        AudioSampleRate = targetRate;

    asynchronous = false;
    enabled = false;
    retro_variable mode{"uzdoom_audio_submission", nullptr};
    if (Environment)
        Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &mode);
    if (Environment && (!mode.value || std::strcmp(mode.value, "sync") != 0))
    {
        retro_audio_callback callback{Drain, SetState};
        asynchronous = Environment(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &callback);
        if (!asynchronous)
            Log(RETRO_LOG_WARN, "Asynchronous audio is unavailable; using synchronous submission.");
    }
}

void ResetAudio()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    ++audioGeneration;
    pending.clear();
    remainder = 0;
}

void SetAudioRenderer(SoundRenderer *renderer)
{
    std::lock_guard<std::mutex> lock(audioMutex);
    ++audioGeneration;
    audioRenderer = renderer;
    pending.clear();
}

void RenderAudio()
{
    // The frontend callback owns both mixing and submission in async mode.
    if (asynchronous)
        return;

    // Advance the mixer on the engine thread
    const double rate = std::isfinite(FrameRate) ? std::max(1.0, FrameRate) : 35.0;
    remainder += AudioSampleRate;
    unsigned frames = unsigned(remainder / rate);
    remainder -= frames * rate;
    std::vector<int16_t> samples(frames * 2, 0);

    if (GSnd) GSnd->RenderSamples(samples.data(), frames);
    {
        std::lock_guard<std::mutex> lock(audioMutex);

        if (!asynchronous || enabled)
        {
            // Bound latency when a frontend stops consuming without notifying us.
            if (pending.size() + frames * 2 > AudioSampleRate * 2)
                pending.clear();

            pending.insert(pending.end(), samples.begin(), samples.begin() + frames * 2);
        }
    }

    // Synchronous submission lets the frontend pace playback and apply its
    // normal audio rate control. Async mode drains on the frontend callback.
    if (!asynchronous)
        Drain();
}
} // namespace Libretro
