#pragma once
#include "libretro.h"
#include <cmath>

namespace Libretro
{
inline unsigned VRPadStick(unsigned stick, bool swapped)
{
    return swapped ? stick ^ 1u : stick;
}

inline bool VRMoveStick(double x, double y, double &forward, double &side)
{
    // Radial filtering alone retains horizontal drift at full forward travel.
    side = std::abs(x) > 0.15 ? x : 0;
    forward = std::abs(y) > 0.15 ? -y : 0;
    const double length = std::hypot(side, forward);
    if (length == 0)
        return false;

    const double scale = std::pow((std::fmin(1.0, length)-0.1)/0.9, 2.2)/std::fmax(1.0, length);
    side *= scale;
    forward *= scale;
    return true;
}

inline void VRMovement(double forward, double side, double heading,
    double &actorForward, double &actorSide)
{
    actorForward = forward*std::cos(heading) + side*std::sin(heading);
    actorSide = side*std::cos(heading) - forward*std::sin(heading);
}

// Keep the tracking-space body heading independent of the actor's queued aim
// commands. Rendering must not infer body rotation from an older actor pose.
struct VRBodyHeading
{
    static constexpr double Pi = 3.14159265358979323846;
    double body = 0;
    double commanded = 0;

    void Reset(double actorYaw)
    {
        body = commanded = actorYaw;
    }

    void Synchronize(double actorYaw, double pendingYaw)
    {
        // Simulation may discard input while frozen or during reactiontime.
        // Keep the body reference fixed, but reconcile the firing direction.
        commanded = std::remainder(actorYaw + pendingYaw, 2*Pi);
    }

    short Aim(double aimOffset, double turn)
    {
        body = std::remainder(body + turn, 2*Pi);
        const double difference = std::remainder(body + aimOffset - commanded, 2*Pi);
        const short command = static_cast<short>(std::lround(difference*32768/Pi));
        commanded = std::remainder(commanded + command*Pi/32768, 2*Pi);
        return command;
    }

    double View(double trackingYaw, double referenceYaw) const
    {
        return body + trackingYaw - referenceYaw;
    }

    double Movement(double headYaw, double referenceYaw) const
    {
        return View(headYaw, referenceYaw) - commanded;
    }
};

struct VRPressEdge
{
    bool held = false;
    bool Changed(bool down)
    {
        const bool changed = down != held;
        held = down;
        return changed;
    }

    bool Press(bool down)
    {
        return Changed(down) && down;
    }
};

struct VRStickState
{
    bool rightReady = true, leftReady = true;
    int weapon = 0;
    int axis = 0;
    struct Actions { int turn; int weapon; float smooth; };

    Actions Poll(float x, float y, bool blocked, bool snap = true)
    {
        if (blocked || (std::abs(x) < 0.4f && std::abs(y) < 0.4f))
        {
            axis = 0;
            Turn(0);
            Weapon(0);
            return {0, 0, 0.0f};
        }

        if (!axis && (std::abs(x) > 0.6f || std::abs(y) > 0.6f))
        {
            axis = std::abs(x) > std::abs(y) ? 1 : 2;
        }

        const float turnX = axis == 1 ? x : 0;
        return {snap ? Turn(turnX) : 0, Weapon(axis == 2 ? y : 0), snap ? 0.0f : turnX};
    }

    int Turn(float x)
    {
        int turn = 0;
        if (x > 0.6f && rightReady)
        {
            turn = -1;
            rightReady = false;
        }

        if (x < -0.6f && leftReady)
        {
            turn = 1;
            leftReady = false;
        }

        if (x < 0.4f)
        {
            rightReady = true;
        }

        if (x > -0.4f)
        {
            leftReady = true;
        }

        return turn;
    }

    int Weapon(float y)
    {
        const int next = y < -0.7f ? -1 : y > 0.7f ? 1 : 0;
        const int change = next != weapon ? next : 0;
        weapon = next;

        return change;
    }
};
}
