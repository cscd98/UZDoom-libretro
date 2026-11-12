#pragma once
#include "libretro.h"

namespace Libretro
{
extern retro_environment_t Environment;
extern retro_input_state_t InputState;
extern retro_hw_render_callback Hardware;
extern unsigned Width, Height, MaxWidth, MaxHeight;
extern bool GLES, UseVulkan;
extern double FrameRate, MeasuredRefresh;
bool IsLoaded();
void UpdateFrameRate();
void Log(enum retro_log_level level, const char *text);
void Present();
void SaveGLState();
void RestoreGLState();
const char *SaveDirectory();
const char *SystemDirectory();
void SetAudioSample(retro_audio_sample_t callback);
void SetAudioBatch(retro_audio_sample_batch_t callback);
void InitAudio();
void ResetAudio();
void RenderAudio();
void RegisterOptions(bool engine);
void ApplyOptions();
void ConfigureResolution();
void QueueVRResolution(unsigned width, unsigned height, bool active);
void ApplyVRResolution(bool allowGrowth = false);
bool AwaitVRResolution();
void ResizeOutput();
bool VRLaserEnabled();
bool VRSmoothTurn();
unsigned VRTurnAngle();
float VRHUDOffset();
unsigned PadBinding(unsigned button);
enum PadAction : unsigned
{
    PadFire = 256,
    PadPreviousWeapon,
    PadNextWeapon,
    PadUse
};
void PollInput();
void ResetInput();
void SetController(unsigned device);
void KeyboardEvent(bool down, unsigned key, uint32_t character, uint16_t modifiers);
}
