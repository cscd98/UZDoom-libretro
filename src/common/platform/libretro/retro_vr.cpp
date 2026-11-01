#include "retro_platform.h"
#include "retro_vr.h"
#include "retro_vr_projection.h"
#include "retro_vr_aim.h"
#include "retro_vr_controls.h"
#include "hw_vrmodes.h"
#include "r_utility.h"
#include "v_video.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_event.h"
#include "d_protocol.h"
#include "d_net.h"
#include "g_levellocals.h"
#include "i_interface.h"
#include "menustate.h"
#include "p_trace.h"
#include "actorinlines.h"
#include "hw_renderstate.h"
#include "flatvertices.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

int P_LibretroPlayerCommandTic();

namespace Libretro
{
namespace
{
bool stereo = false, immersive = false, referenceValid = false;
bool vrInputProfile = false;
VRBodyHeading bodyHeading;
AActor *bodyActor = nullptr;
unsigned renderedEyes = 0;
unsigned movementTraceTicks = 0, movementTraceSamples = 0;
retro_vr_frame_state frame{};
float origin[3]{}, headPosition[3]{}, headOrientation[4]{};
bool controllerAimValid = false;
retro_vr_pose controllerAim{}, rawControllerAim{};
float aimCorrection[4]{0, 0, 0, 1};
float weaponPosition[3]{}, weaponOrientation[4]{0,0,0,1};
unsigned currentEye = 0;
int overlayEye = -1;
double referenceYaw = 0, headYaw = 0, headPitch = 0, appliedYaw = 0;
constexpr double UnitsPerMeter = 41.0;
constexpr double Pi = 3.14159265358979323846;

// Tracking coordinates are right-handed: +X right, +Y up, -Z forward.
// Decompose R_y(yaw) R_x(pitch) R_z(roll), preserving eye cant and roll.
void Angles(const float *q, double &yaw, double &pitch, double &roll)
{
    const double x=q[0], y=q[1], z=q[2], w=q[3];
    pitch = std::asin(std::clamp(2*(w*x-y*z), -1.0, 1.0));
    yaw = std::atan2(2*(w*y+x*z), 1-2*(x*x+y*y));
    roll = std::atan2(2*(w*z+x*y), 1-2*(x*x+z*z));
}

bool ValidEye(retro_vr_eye_state &eye)
{
    double norm = 0;
    for (float v : eye.position) if (!std::isfinite(v))
        return false;

    for (float v : eye.orientation)
    {
        if (!std::isfinite(v))
            return false;
        norm += v*v;
    }

    if (norm < 0.5 || norm > 1.5)
        return false;

    for (float &v : eye.orientation)
        v /= std::sqrt(norm);

    for (float v : eye.fov_tan)
        if (!std::isfinite(v) || v <= 0 || v > 100)
            return false;

    return true;
}
}

void ResetVR()
{
    stereo = immersive = referenceValid = vrInputProfile = false;
    bodyActor = nullptr;
    renderedEyes = 0;
    movementTraceTicks = movementTraceSamples = 0;
    overlayEye = -1;
    appliedYaw = 0;
    controllerAimValid = false;
    aimCorrection[0] = aimCorrection[1] = aimCorrection[2] = 0;
    aimCorrection[3] = 1;
}

int StereoMode()
{
    return stereo ? VR_SIDEBYSIDESQUISHED : VR_MONO;
}

bool VRActive()
{
    return stereo;
}

bool VRImmersive()
{
    return immersive;
}

bool VRControlsActive()
{
    return vrInputProfile;
}

void CalibrateVRAim()
{
    if (!immersive || !controllerAimValid)
        return;

    AimCalibration(rawControllerAim.orientation, headOrientation, aimCorrection);
    MultiplyOrientation(rawControllerAim.orientation, aimCorrection, controllerAim.orientation);

    Log(RETRO_LOG_INFO, "VR aim centred on headset direction.\n");
}

void DrawVRLaser(const FRenderViewpoint &view, FRenderState &state)
{
    if (!stereo || !VRLaserEnabled() || gamestate != GS_LEVEL || menuactive != MENU_Off)
        return;

    auto actor = players[consoleplayer].mo;
    if (!actor || view.camera != actor || (players[consoleplayer].cheats & CF_CHASECAM))
        return;

    // Trace the weapon's aim without firing, spawning actors or activating lines.
    const auto yaw = actor->Angles.Yaw;
    const auto pitch = actor->Angles.Pitch;
    DVector3 direction(pitch.Cos()*yaw.Cos(), pitch.Cos()*yaw.Sin(), -pitch.Sin());
    auto start = actor->PosAtZ(actor->Center() - actor->Floorclip + actor->AttackOffset());

    if (immersive && controllerAimValid)
    {
        // Use the same pose and tracking-to-world transform as the visible
        // weapon. Actor pitch is a simulated input result, not the current
        // controller ray, and its origin is not the tracked hand position.
        double eyeYaw, eyePitch, eyeRoll;
        Angles(frame.eyes[currentEye].orientation, eyeYaw, eyePitch, eyeRoll);
        const double bodyYaw = view.Angles.Yaw.Radians() - eyeYaw;
        const double stretch = view.ViewLevel->info->pixelstretch;

        float forward[3];
        AimForward(weaponOrientation, forward);
        double world[3];

        TrackingVector(forward, bodyYaw, stretch, world);
        direction = DVector3(world[0], world[1], world[2]).Unit();

        float offset[3];
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            offset[axis] = weaponPosition[axis] - frame.eyes[currentEye].position[axis];
        }

        TrackingVector(offset, bodyYaw, stretch, world);
        start = view.Pos + DVector3(world[0], world[1], world[2]) * UnitsPerMeter;
    }

    FTraceResults hit{};
    if (!Trace(start, immersive && controllerAimValid ? view.ViewLevel->PointInSector(start) : actor->Sector, direction, 4000, MF_SHOOTABLE, ML_BLOCKEVERYTHING,
        actor, hit, TRACE_NoSky | TRACE_PortalRestrict) || hit.HitType == TRACE_HitNone)
        return;

    // A world-space, depth-tested dot gives each eye the correct parallax.
    const auto center = hit.HitPos - hit.HitVector * 0.15;
    const auto eyeYaw = view.Angles.Yaw;
    const auto eyePitch = view.Angles.Pitch;
    const DVector3 right(eyeYaw.Sin(), -eyeYaw.Cos(), 0);
    const DVector3 up(eyePitch.Sin()*eyeYaw.Cos(), eyePitch.Sin()*eyeYaw.Sin(), eyePitch.Cos());
    const double radius = std::clamp(hit.Distance * 0.0015, 0.2, 2.0);
    constexpr int segments = 12;
    screen->mVertexData->Map();

    auto vertices = screen->mVertexData->AllocVertices(segments + 2);
    vertices.first[0].Set(center.X, center.Z, center.Y, 0, 0);

    for (int i = 0; i <= segments; ++i)
    {
        const double angle = 2 * Pi * i / segments;
        const auto point = center + (right * std::cos(angle) + up * std::sin(angle)) * radius;
        vertices.first[i+1].Set(point.X, point.Z, point.Y, 0, 0);
    }

    screen->mVertexData->Unmap();

    state.SetVertexBuffer(screen->mVertexData);
    state.EnableModelMatrix(false);
    state.EnableTexture(false);
    state.SetEffect(EFF_NONE);
    state.SetLightIndex(-1);
    state.SetObjectColor(0xffffffff);
    state.SetAddColor(0);
    state.SetDynLight(0, 0, 0);
    state.EnableSplit(false);
    state.ClearClipSplit();
    state.SetSoftLightLevel(255);
    state.SetColor(1, 0, 0, 1);
    state.SetRenderStyle(STYLE_Translucent);
    state.SetDepthMask(false);
    state.EnableDepthTest(true);
    state.Draw(DT_TriangleFan, vertices.second, segments + 2);
    state.SetDepthMask(true);
    state.SetNoSoftLightLevel();
    state.ResetColor();
    state.EnableTexture(true);
}

void BeginVRFrame()
{
    const bool hadInputProfile = vrInputProfile;
    vrInputProfile = false;
    controllerAimValid = false;
    stereo = immersive = false;
    renderedEyes = 0;
    overlayEye = -1;

    retro_variable option{"uzdoom_vr", nullptr};
    if (!Environment || UseVulkan || !Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) ||
        !option.value || !std::strcmp(option.value, "off"))
    {
        referenceValid = false;
        return;
    }

    unsigned status = 0;
    Environment(RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, &status);
    // Once a headset session is active, transient tracking/status failures must
    // not switch the same stick to the ordinary gamepad bindings.
    vrInputProfile = !std::strcmp(option.value, "immersive") &&
        (hadInputProfile || (status & RETRO_VIDEO_VIEWS_STATUS_HMD));

    const unsigned required = RETRO_VIDEO_VIEWS_STATUS_PRESENTS | RETRO_VIDEO_VIEWS_STATUS_STEREO;
    if ((status & required) != required)
    {
        if (!vrInputProfile)
            referenceValid = false;
        return;
    }

    stereo = true;
    if (std::strcmp(option.value, "immersive"))
    {
        referenceValid = false;
        return;
    }

    if (!(status & RETRO_VIDEO_VIEWS_STATUS_HMD) ||
        !Environment(RETRO_ENVIRONMENT_GET_VR_FRAME_STATE, &frame) ||
        !ValidEye(frame.eyes[0]) || !ValidEye(frame.eyes[1]))
    {
        // Never submit stale poses. Preserve the tracking origin across dropouts.
        stereo = false;
        return;
    }

    immersive = true;
    // The midpoint is also usable on frontends exposing only the eye callback.
    auto &orientation = headOrientation;

    for (unsigned i = 0; i < 3; ++i)
        headPosition[i] = (frame.eyes[0].position[i] + frame.eyes[1].position[i]) * 0.5f;
    double dot = 0, norm = 0;

    for (unsigned i = 0; i < 4; ++i)
        dot += frame.eyes[0].orientation[i] * frame.eyes[1].orientation[i];

    for (unsigned i = 0; i < 4; ++i)
    {
        orientation[i] = frame.eyes[0].orientation[i] + (dot < 0 ? -1 : 1) * frame.eyes[1].orientation[i];
        norm += orientation[i] * orientation[i];
    }

    for (float &v : orientation)
        v /= std::sqrt(norm);

    double roll;
    Angles(orientation, headYaw, headPitch, roll);

    if (!referenceValid || (frame.flags & RETRO_VR_FRAME_RECENTERED))
    {
        std::copy(headPosition, headPosition + 3, origin);

        if (bodyActor)
        {
            bodyHeading.body += headYaw - referenceYaw;
        }

        referenceYaw = headYaw;
        referenceValid = true;
    }

    retro_vr_controller_poses controllers{};

    if (Environment(RETRO_ENVIRONMENT_GET_VR_CONTROLLER_POSES, &controllers))
    {
        controllerAimValid = ControllerAim(controllers, rawControllerAim);
        controllerAim = rawControllerAim;

        if (controllerAimValid)
        {
            MultiplyOrientation(rawControllerAim.orientation, aimCorrection, controllerAim.orientation);
        }
    }
}

void ApplyVRInput(usercmd_t &command)
{
    if (gamestate != GS_LEVEL || paused || menuactive != MENU_Off)
        return;

    auto &player = players[consoleplayer];

    if (!player.mo || player.camera != player.mo)
        return;

    if (!immersive)
    {
        ConsumeVRTurn();

        if (vrInputProfile)
        {
            // Pause locomotion through tracking loss instead of accepting legacy
            // axes or changing the tracking reference while commands are pending.
            command.forwardmove = command.sidemove = command.yaw = command.pitch = 0;
            return;
        }

        if (bodyActor)
        {
            command.yaw = static_cast<short>(command.yaw + bodyHeading.Aim(0, 0));
            bodyActor = nullptr;
        }

        appliedYaw = 0;

        return;
    }

    if (bodyActor != player.mo)
    {
        bodyActor = player.mo;
        bodyHeading.Reset(player.mo->Angles.Yaw.Radians());
    }

    double yaw = referenceYaw, pitch = 0, roll = 0;

    if (controllerAimValid)
    {
        Angles(controllerAim.orientation, yaw, pitch, roll);
    }

    const usercmd_t legacyCommand = command;
    const double targetYaw = controllerAimValid ? std::remainder(yaw-referenceYaw, 2*Pi) : appliedYaw;
    double forward = 0, side = 0;
    bool moving = false;

    if (InputState)
    {
        const unsigned movementStick = VRPadStick(RETRO_DEVICE_INDEX_ANALOG_LEFT, VRSticksSwapped());
        const double x = InputState(0, RETRO_DEVICE_ANALOG, movementStick, RETRO_DEVICE_ID_ANALOG_X)/32768.0;
        const double y = InputState(0, RETRO_DEVICE_ANALOG, movementStick, RETRO_DEVICE_ID_ANALOG_Y)/32768.0;
        moving = VRMoveStick(x, y, forward, side);
    }

    const double turn = ConsumeVRTurn()*Pi/180 + (moving ? 0 : command.yaw*Pi/32768);
    // G_BuildTiccmd is called with ClientTic already incremented. The actor
    // may already include predicted ticks; include only older pending commands
    // so generating several commands ahead does not apply the correction twice.
    double pendingYaw = 0;
    const int currentCommandTic = ClientTic - 1;

    for (int tic = std::max(P_LibretroPlayerCommandTic(), currentCommandTic - LOCALCMDTICS + 1);
        tic < currentCommandTic; ++tic)
    {
        pendingYaw += LocalCmds[tic % LOCALCMDTICS].yaw * Pi/32768;
    }

    bodyHeading.Synchronize(player.mo->Angles.Yaw.Radians(), pendingYaw);
    command.yaw = bodyHeading.Aim(targetYaw, turn);

    if (moving)
    {
        double actorForward, actorSide;
        VRMovement(forward, side, bodyHeading.Movement(headYaw, referenceYaw), actorForward, actorSide);
        command.forwardmove = std::clamp<long>(std::lround(actorForward*0x3200), -0x3200, 0x3200);
        command.sidemove = std::clamp<long>(std::lround(actorSide*0x3200), -0x3200, 0x3200);
        command.buttons &= ~(BT_FORWARD | BT_BACK | BT_MOVELEFT | BT_MOVERIGHT | BT_LEFT | BT_RIGHT | BT_STRAFE);
    }

    if (controllerAimValid)
    {
        command.pitch = static_cast<short>(std::clamp(std::lround(
            (pitch + player.mo->Angles.Pitch.Radians()) * 32768/Pi), -32767l, 32767l));
    }

    appliedYaw = targetYaw;

    // Bounded on-device diagnostic: correlate frontend axes, generated commands
    // and observed actor motion while reproducing the reported movement fault.
    if (InputState && ++movementTraceTicks % 7 == 0 && movementTraceSamples < 300)
    {
        ++movementTraceSamples;
        int axes[4];
        for (unsigned stick = 0; stick < 2; ++stick)
        {
            for (unsigned axis = 0; axis < 2; ++axis)
            {
                axes[stick*2+axis] = InputState(0, RETRO_DEVICE_ANALOG, stick, axis);
            }
        }

        unsigned buttons = 0;
        for (unsigned button = 0; button < 16; ++button)
        {
            if (InputState(0, RETRO_DEVICE_JOYPAD, 0, button))
            {
                buttons |= 1u << button;
            }
        }

        char message[768];
        std::snprintf(message, sizeof(message),
            "VRMOVE tick=%d sticks=(%d,%d)/(%d,%d) move_stick=%u buttons=%04x "
            "legacy=(%d,%d,%d) cmd=(%d,%d,%d) "
            "head=%.2f ref=%.2f aim=%.2f body=%.2f queued=%.2f actor=%.2f "
            "pos=(%.3f,%.3f) vel=(%.3f,%.3f) tracked=%d\n",
            gametic, axes[0], axes[1], axes[2], axes[3],
            VRPadStick(RETRO_DEVICE_INDEX_ANALOG_LEFT, VRSticksSwapped()), buttons,
            int(legacyCommand.forwardmove), int(legacyCommand.sidemove), int(legacyCommand.yaw),
            int(command.forwardmove), int(command.sidemove), int(command.yaw),
            headYaw*180/Pi, referenceYaw*180/Pi, targetYaw*180/Pi,
            bodyHeading.body*180/Pi, bodyHeading.commanded*180/Pi, player.mo->Angles.Yaw.Degrees(),
            player.mo->X(), player.mo->Y(), player.mo->Vel.X, player.mo->Vel.Y, int(controllerAimValid));

        Log(RETRO_LOG_INFO, message);
    }
}

bool ApplyVREye(unsigned eyeIndex, FRenderViewpoint &view, VSMatrix &projection)
{
    if (!immersive || eyeIndex >= 2)
        return false;

    currentEye = eyeIndex;
    const auto &eye = frame.eyes[eyeIndex];
    double yaw, pitch, roll;
    Angles(eye.orientation, yaw, pitch, roll);

    if (bodyActor != view.camera)
    {
        bodyActor = view.camera;
        bodyHeading.Reset(view.camera->Angles.Yaw.Radians());
    }

    const double bodyYaw = bodyHeading.body - referenceYaw;
    // Without motion-controller poses, retain independent mouse/stick aiming.
    // Its neutral tracking-space heading is the recentered heading, not the HMD.
    AimOrientation(weaponOrientation, referenceYaw + appliedYaw, -view.Angles.Pitch.Radians());
    std::copy(headPosition, headPosition + 3, weaponPosition);

    if (controllerAimValid)
    {
        std::copy(controllerAim.orientation, controllerAim.orientation + 4, weaponOrientation);
        if (controllerAim.flags & RETRO_VR_POSE_POSITION_VALID)
            std::copy(controllerAim.position, controllerAim.position + 3, weaponPosition);
    }

    const double right = (eye.position[0]-origin[0]) * UnitsPerMeter;
    const double forward = -(eye.position[2]-origin[2]) * UnitsPerMeter;
    view.Pos.X += forward*std::cos(bodyYaw) + right*std::sin(bodyYaw);
    view.Pos.Y += forward*std::sin(bodyYaw) - right*std::cos(bodyYaw);
    view.Pos.Z += (eye.position[1]-origin[1]) * UnitsPerMeter / view.ViewLevel->info->pixelstretch;
    view.Angles.Yaw = DAngle::fromRad(bodyYaw + yaw);
    view.Angles.Pitch = DAngle::fromRad(-pitch);
    view.Angles.Roll = DAngle::fromRad(-roll);
    view.HWAngles.Pitch = FAngle::fromRad(-pitch);
    view.HWAngles.Roll = FAngle::fromRad(-roll);

    // Conservative culling includes asymmetric frusta and rolled views.
    view.FieldOfView = DAngle::fromDeg(179);
    const float nearPlane = screen->GetZNear(), farPlane = screen->GetZFar();
    EyeProjection(projection, eye, nearPlane, farPlane);
    renderedEyes |= 1u << eyeIndex;

    return true;
}

void SetVROverlayEye(int eye)
{
    overlayEye = eye;
}

bool ApplyVRHUD(VSMatrix &projection, int width, int height, bool weapon)
{
    if (!immersive || (!weapon && overlayEye < 0))
        return false;

    unsigned eye = weapon ? currentEye : unsigned(overlayEye);
    if (eye >= 2 || !(renderedEyes & (1u << eye)))
        return false;

    HeadPlaneProjection(projection, frame.eyes[eye],
        weapon ? weaponPosition : headPosition, weapon ? weaponOrientation : headOrientation,
        float(width), float(height), weapon && controllerAimValid ? 0.35f : weapon ? 1.0f : 1.5f,
        weapon ? 0.0f : VRHUDOffset());

    return true;
}

void SubmitVRViews()
{
    if (!Environment)
        return;

    retro_video_view views[2] = {
        {0, 0, Width/2, Height, 0, RETRO_VIDEO_VIEW_EYE_LEFT, float(Width)/Height},
        {Width/2, 0, Width/2, Height, 0, RETRO_VIDEO_VIEW_EYE_RIGHT, float(Width)/Height}
    };

    retro_video_views map{};
    map.views = views;
    map.num_views = stereo ? 2 : 0;

    // Title screens and menus without a rendered scene remain flat stereo screens.
    map.flags = immersive && renderedEyes == 3 ? RETRO_VIDEO_VIEWS_FLAG_TRACKED : 0;
    map.reference_space = RETRO_VR_REFERENCE_SPACE_LOCAL;
    Environment(RETRO_ENVIRONMENT_SET_VIDEO_VIEWS, &map);

    retro_variable option{"uzdoom_vr", nullptr};
    const bool requested = !UseVulkan && Environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) &&
        option.value && std::strcmp(option.value, "off") != 0;
    QueueVRResolution(map.recommended_view_width, map.recommended_view_height, requested);
}
}
