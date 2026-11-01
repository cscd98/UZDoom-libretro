#pragma once

struct FRenderViewpoint;
struct usercmd_t;
class VSMatrix;
class FRenderState;

namespace Libretro
{
void BeginVRFrame();
void SubmitVRViews();
void ResetVR();
int StereoMode();
bool VRActive();
bool VRImmersive();
bool VRControlsActive();
bool VRSticksSwapped();
void CalibrateVRAim();
double ConsumeVRTurn();
double VRSmoothTurnRate();
void DrawVRLaser(const FRenderViewpoint &view, FRenderState &state);
void SetVROverlayEye(int eye);
bool ApplyVRHUD(VSMatrix &projection, int width, int height, bool weapon = false);
void ApplyVRInput(usercmd_t &command);
bool ApplyVREye(unsigned eye, FRenderViewpoint &view, VSMatrix &projection);
}
