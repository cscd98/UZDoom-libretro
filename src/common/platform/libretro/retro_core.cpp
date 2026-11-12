#if defined(__APPLE__)
#include <strings.h>
#include <TargetConditionals.h>
#endif
#include "retro_platform.h"
#include "retro_vr.h"
#include "retro_vfs.h"
#include "retro_vulkan.h"
#include "fs_files.h"
#include "m_argv.h"
#include "cmdlib.h"
#include "engineerrors.h"
#include "c_dispatch.h"
#include "i_sound.h"
#include "i_time.h"
#include <cmath>
#include <chrono>
#include <array>
#include <algorithm>
#include "retro_context.h"
#ifdef __ANDROID__
#include "retro_frontend_stack.h"
#endif
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

int GameMain();
void LibretroUnloadSndFile();
extern FArgs *Args;
extern void HWShutdownRenderPool();

namespace Libretro
{
retro_environment_t Environment = nullptr;
retro_input_state_t InputState = nullptr;
retro_hw_render_callback Hardware{};
unsigned Width = 640, Height = 480, MaxWidth = 3840, MaxHeight = 2160;
bool GLES = false, UseVulkan = false;
double FrameRate = 35.0, MeasuredRefresh = 0;


namespace
{
retro_video_refresh_t video = nullptr;
retro_input_poll_t poll = nullptr;
retro_log_printf_t logger = nullptr;
RetroContext frontendContext{}, engineContext{};
#ifdef __ANDROID__
RetroFrontendStack frontendStack(frontendContext, engineContext);
#endif
std::unique_ptr<unsigned char[]> stack;
constexpr size_t StackSize = 16 * 1024 * 1024;
bool loaded = false, started = false, finished = false, stopping = false, contextReady = false;
bool presented = false;
bool resetRequested = false;
unsigned resolutionWaitFrames = 0;
std::string content, systemDirectory, saveDirectory;

void ResumeEngine()
{
#ifdef __ANDROID__
    frontendStack.Resume();
#else
    SwitchContext(&frontendContext, &engineContext);
#endif
}

void EngineEntry()
{
    try
    {
        progdir = (systemDirectory + "/").c_str();
        std::vector<std::string> arguments = {
            "uzdoom_libretro", "-iwad", content,
            "-config", saveDirectory + "/uzdoom-libretro.ini",
            "-savedir", saveDirectory,
            "+vid_preferbackend", UseVulkan ? "1" : GLES ? "2" : "0",
            "+vid_fullscreen", "false", "+vid_defwidth", std::to_string(Width), "+vid_defheight", std::to_string(Height),
            "+cl_capfps", "true", "+showendoom", "0",
            "+snd_samplerate", "48000"
        };

        std::vector<const char *> argv;
        for (auto &arg : arguments)
            argv.push_back(arg.c_str());

        Args = new FArgs(static_cast<int>(argv.size()), argv.data());

        GameMain();
    }
    catch (const std::exception &error)
    {
        Log(RETRO_LOG_ERROR, error.what());
    }
    catch (...)
    {
        Log(RETRO_LOG_ERROR, "Unhandled engine exception");
    }

    finished = true;
#if defined(__ANDROID__) || defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
    SwitchContext(&engineContext, &frontendContext);
    std::abort();
#endif
}

void Stop()
{
    if (started && !finished)
    {
        stopping = true;
        ResumeEngine();
    }

    DestroyContext(engineContext, frontendContext);
    stack.reset();
    started = false;
    loaded = false;
    ResetInput();
    ResetVR();
    resetRequested = false;
    ResetAudio();
}

void ContextReset()
{
    contextReady = true;
#ifdef HAVE_VULKAN
    if (UseVulkan)
        contextReady = ResetVulkan();
#endif
    if (!contextReady)
        Log(RETRO_LOG_ERROR, "Vulkan context negotiation failed");
}

void ContextDestroy()
{
    // Destroy engine GPU resources while the frontend context is still current.
    if (started)
        Stop();

    contextReady = false;
}

} // namespace

// Run at the largest whole fraction of the target refresh rate within 90 Hz, falling back to one tic per frame.
static double TargetFrameRate()
{
    float refresh = 0;
    const bool got = Environment && Environment(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &refresh);

    if (MeasuredRefresh > 0 && VRActive())
        refresh = float(MeasuredRefresh);

    if ((got || MeasuredRefresh > 0) && refresh >= 35.0f)
        return refresh / std::max(1.0, std::ceil(refresh / 90.0 - 0.001));

    return 35.0;
}

// Median run interval over a window, snapped to a standard headset rate.
static void MeasureRefresh()
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last;
    static std::vector<double> intervals;
    const auto now = Clock::now();

    if (!started || !VRActive())
    {
        intervals.clear();
        last = {};
        return;
    }

    if (last != Clock::time_point{})
        intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());

    last = now;

    if (intervals.size() < 120)
        return;

    std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2, intervals.end());
    const double measured = 1000.0 / intervals[intervals.size() / 2];
    intervals.clear();

    double rate = measured;
    for (double standard : {60.0, 72.0, 80.0, 90.0, 120.0, 144.0})
        if (std::abs(measured - standard) < standard * 0.04)
            rate = standard;

    if (std::abs(rate - MeasuredRefresh) > 0.5)
        MeasuredRefresh = rate;
}

void UpdateFrameRate()
{
    MeasureRefresh();
    const double fps = TargetFrameRate();

    if (std::abs(fps - FrameRate) < 0.001)
        return;

    FrameRate = fps;
    I_RetroSetFrameRate(fps == 35.0 ? 0 : fps);

    if (started && Environment)
    {
        retro_system_av_info info{{Width, Height, MaxWidth, MaxHeight, float(Width) / Height}, {FrameRate, 48000.0}};
        Environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
    }
}

void Log(retro_log_level level, const char *text)
{
    if (logger)
        logger(level, "%s", text);
#ifndef _WIN32
    else
        std::fprintf(stderr, "%s", text);
#endif
}

void Present()
{
    presented = true;
}

const char *SaveDirectory()
{
    return saveDirectory.c_str();
}

const char *SystemDirectory()
{
    return systemDirectory.c_str();
}

bool IsLoaded()
{
    return loaded;
}

} // namespace Libretro

void LibretroEndFrame()
{
    using namespace Libretro;
    SaveGLState();
    SwitchContext(&engineContext, &frontendContext);
    RestoreGLState();
    if (stopping)
        throw CExitEvent(0);

    if (resetRequested)
    {
        resetRequested = false;
        AddCommandString("restart");
    }
}

using namespace Libretro;

extern "C"
{
unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    Environment = cb;
    if (!cb)
        return;

    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
        RegisterOptions(false);

    retro_keyboard_callback keyboard{KeyboardEvent};
    cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);

    bool noGame = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { SetAudioSample(cb); }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { SetAudioBatch(cb); }
void retro_set_input_poll(retro_input_poll_t cb) { poll = cb; }
void retro_set_input_state(retro_input_state_t cb) { InputState = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { if (port == 0) SetController(device); }

void retro_init()
{
    if (!Environment)
        return;

    retro_log_callback logging{};
    if (Environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        logger = logging.log;

    InitVFS(Environment);
    InitAudio();
}

void retro_deinit()
{
    HWShutdownRenderPool();
    Stop();
    LibretroUnloadSndFile();
    InitVFS(nullptr);
    logger = nullptr;
    contextReady = false;
}

void retro_get_system_info(retro_system_info *info)
{
    *info = {"UZDoom", "experimental", "wad|iwad", true, false};
}

void retro_get_system_av_info(retro_system_av_info *info)
{
    *info = {{Width, Height, MaxWidth, MaxHeight, float(Width) / Height}, {TargetFrameRate(), 48000.0}};
}

bool retro_load_game(const retro_game_info *info)
{
    if (!Environment || loaded || !info || !info->path || !*info->path)
        return false;

    ConfigureResolution();
    resolutionWaitFrames = 0;
    FileSys::FileReader file;
    if (!file.OpenFile(info->path))
        return false;

    const char *directory = nullptr;
    if (!Environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) || !directory || !*directory)
    {
        Log(RETRO_LOG_ERROR, "Set a frontend system directory for optional resources. Required PK3s are embedded.");
        return false;
    }

    systemDirectory = directory;
    directory = nullptr;

    if (!Environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &directory) || !directory || !*directory)
    {
        Log(RETRO_LOG_ERROR, "A writable frontend save directory is required.");
        return false;
    }

    saveDirectory = directory;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    retro_hw_context_type preferred = RETRO_HW_CONTEXT_OPENGLES2;
#else
    retro_hw_context_type preferred = RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
    Environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred);
    GLES = preferred == RETRO_HW_CONTEXT_OPENGLES2 || preferred == RETRO_HW_CONTEXT_OPENGLES3
        || preferred == RETRO_HW_CONTEXT_OPENGLES_VERSION;
    UseVulkan = preferred == RETRO_HW_CONTEXT_VULKAN;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    if (preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGL_CORE)
    {
        GLES = true;
    }
#endif

    retro_pixel_format pixel = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!Environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel))
        return false;

    Hardware = {};
    Hardware.context_type = UseVulkan ? RETRO_HW_CONTEXT_VULKAN : GLES ? RETRO_HW_CONTEXT_OPENGLES2 : RETRO_HW_CONTEXT_OPENGL_CORE;
    Hardware.version_major = GLES ? 2 : 3;
    Hardware.version_minor = GLES ? 0 : 3;
#ifdef _WIN32
    if (!UseVulkan && !GLES)
    {
        // RetroArch's GL menu fonts use legacy luminance/alpha textures.
        // WGL interprets a 3.1+ version request as a core profile, even when
        // context_type is OPENGL. Request its default compatibility context;
        // the engine still checks that the resulting GL version is >= 3.3.
        Hardware.context_type = RETRO_HW_CONTEXT_OPENGL;
        Hardware.version_major = Hardware.version_minor = 0;
    }
#endif
    Hardware.context_reset = ContextReset;
    Hardware.context_destroy = ContextDestroy;
    Hardware.depth = true;
    Hardware.stencil = true;
    Hardware.bottom_left_origin = true;
    Hardware.cache_context = false;
    contextReady = false;

    if (!Environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &Hardware))
        return false;
#ifdef HAVE_VULKAN
    if (UseVulkan && !NegotiateVulkan())
        return false;
#endif
    content = info->path;
    finished = stopping = started = false;
    loaded = true;

    return true;
}

void retro_unload_game()
{
    Stop();
}

void retro_run()
{
    if (!loaded || !contextReady || finished)
        return;


    ApplyVRResolution(!started);

    if (!loaded || !contextReady)
        return;

    if (poll)
        poll();

    if (!started)
    {
        // Negotiate the headset target before creating engine GPU resources.
        BeginVRFrame();
        SubmitVRViews();

        if (AwaitVRResolution() && resolutionWaitFrames++ < 120)
        {
            // Let the frontend advance its headset session before starting the engine.
            video(nullptr, Width, Height, 0);
            return;
        }

        if (AwaitVRResolution())
        {
            Log(RETRO_LOG_WARN, "Frontend did not provide a headset resolution during startup; using the fallback size.\n");
        }

        ApplyVRResolution(true);

        if (!loaded || !contextReady)
            return;
    }

    BeginVRFrame();
    UpdateFrameRate();

    const bool frontendStateSaved = SaveFrontendGLState(!started);

    if (started)
    {
        ApplyOptions();
        PollInput(); 
    }
    else
    {
#ifndef _WIN32
        stack = std::make_unique<unsigned char[]>(StackSize);
#endif
        try
        {
            InitContext(engineContext, stack.get(), StackSize, EngineEntry, frontendContext);
        }
        catch (const std::exception &error)
        {
            if (frontendStateSaved)
                RestoreFrontendGLState();
            Log(RETRO_LOG_ERROR, error.what());
            finished = true;
            Environment(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);

            return;
        }
        started = true;
    }

    presented = false;
    I_RetroAdvanceFrame();
    ResumeEngine();

    if (frontendStateSaved)
        RestoreFrontendGLState();

    if (finished)
    {
        Environment(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        return;
    }

    RenderAudio();

    SubmitVRViews();

    if (video)
        video(presented ? RETRO_HW_FRAME_BUFFER_VALID : nullptr, Width, Height, 0);
}

void retro_reset()
{
    if (started && !finished)
        resetRequested = true;
}

size_t retro_serialize_size()
{
    return 0;
}

bool retro_serialize(void *, size_t)
{
    return false;
}

bool retro_unserialize(const void *, size_t)
{
    return false;
}

void retro_cheat_reset()
{
}

void retro_cheat_set(unsigned, bool, const char *)
{
}

bool retro_load_game_special(unsigned, const retro_game_info *, size_t)
{
    return false;
}
unsigned retro_get_region()
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned)
{
    return nullptr;
}

size_t retro_get_memory_size(unsigned)
{
    return 0;
}
} // extern "C"
