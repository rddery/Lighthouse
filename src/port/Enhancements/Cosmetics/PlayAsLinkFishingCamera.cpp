#include "PlayAsLinkFishingCamera.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include "functions.h"
#include "bk_time.h"

void func_802C0490(f32 dst[3]); // the point the follow camera orbits (about Link's chest)
}

namespace {

constexpr float kPi = 3.14159265358979f;

// Link is about 156 units tall here
constexpr float kChest = 105.0f;

// How quickly the camera settles on its goal, per second: the view changes (rod out, hooked, caught) glide
constexpr float kPositionRate = 2.6f;
constexpr float kFocusRate = 3.6f;

// Rod out: close behind Link and a little to the side, high enough to see over him to the water
constexpr float kRodBehind = 320.0f;
constexpr float kRodSide = 95.0f;
constexpr float kRodHeight = 200.0f;
// A flying lure is high above the water, so the view keeps to a level a little below Link's chest
constexpr float kRodFocusHeight = 70.0f;
constexpr float kRodFocusShare = 0.55f; // how far from Link toward the lure the camera looks
constexpr float kMaxAimDistance = 700.0f;

// Fight: back by an amount that grows with how far the fish is, so it stays in the picture
constexpr float kFightBehind = 250.0f;
constexpr float kFightBehindPerDistance = 0.3f;
constexpr float kFightSide = 70.0f;
constexpr float kFightHeight = 260.0f;
constexpr float kFightHeightPerDistance = 0.12f;

// Catch: in front of Link, turned toward his side, level with the fish he holds up
constexpr float kCatchAhead = 290.0f;
constexpr float kCatchAngle = 38.0f; // degrees round from straight ahead
constexpr float kCatchHeight = 145.0f;
constexpr float kCatchFocusHeight = 88.0f;

struct Camera {
    bool active = false;
    bool haveRequest = false;
    bool smoothValid = false;
    PlayAsLinkFishingCamera::Request request;
    // The eye, as an angle round Link, a distance from him and a height above his feet: the camera glides round him
    // rather than through him when the view changes
    float angle = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    float focus[3] = {};
};
Camera sCamera;

void Forward(float yawDegrees, float out[2]) {
    const float yaw = yawDegrees * kPi / 180.0f;
    out[0] = std::sin(yaw);
    out[1] = std::cos(yaw);
}

// The eye and the point it looks at for a request
void Goal(const PlayAsLinkFishingCamera::Request& r, float eye[3], float focus[3]) {
    using PlayAsLinkFishingCamera::View;
    float forward[2];
    Forward(r.yaw, forward);
    const float chest[3] = { r.link[0], r.link[1] + kChest, r.link[2] };

    if (r.view == View::Catch) {
        float ahead[2];
        Forward(r.yaw + kCatchAngle, ahead);
        eye[0] = r.link[0] + ahead[0] * kCatchAhead;
        eye[1] = r.link[1] + kCatchHeight;
        eye[2] = r.link[2] + ahead[1] * kCatchAhead;
        focus[0] = r.link[0];
        focus[1] = r.link[1] + kCatchFocusHeight;
        focus[2] = r.link[2];
        return;
    }

    // Link to the aimed spot, level
    float toAim[2] = { r.aim[0] - r.link[0], r.aim[2] - r.link[2] };
    float distance = std::sqrt(toAim[0] * toAim[0] + toAim[1] * toAim[1]);
    if (distance < 60.0f) {
        toAim[0] = forward[0];
        toAim[1] = forward[1];
        distance = 60.0f;
    } else {
        toAim[0] /= distance;
        toAim[1] /= distance;
    }

    if (r.view == View::Rod) {
        // The camera sits behind Link along where he faces, and looks toward the lure
        distance = std::min(distance, kMaxAimDistance);
        const float side[2] = { forward[1], -forward[0] };
        eye[0] = r.link[0] - forward[0] * kRodBehind + side[0] * kRodSide;
        eye[1] = r.link[1] + kRodHeight;
        eye[2] = r.link[2] - forward[1] * kRodBehind + side[1] * kRodSide;
        focus[0] = chest[0] + toAim[0] * distance * kRodFocusShare;
        focus[1] = r.link[1] + kRodFocusHeight;
        focus[2] = chest[2] + toAim[1] * distance * kRodFocusShare;
        return;
    }

    // Fight: behind Link on the line from the fish through him; look at the middle of the two
    const float side[2] = { toAim[1], -toAim[0] };
    const float behind = kFightBehind + distance * kFightBehindPerDistance;
    eye[0] = r.link[0] - toAim[0] * behind + side[0] * kFightSide;
    eye[1] = r.link[1] + kFightHeight + distance * kFightHeightPerDistance;
    eye[2] = r.link[2] - toAim[1] * behind + side[1] * kFightSide;
    focus[0] = (chest[0] + r.aim[0]) * 0.5f;
    focus[1] = (chest[1] + r.aim[1]) * 0.5f;
    focus[2] = (chest[2] + r.aim[2]) * 0.5f;
}

void Approach(float value[3], const float goal[3], float rate, float dt) {
    const float t = 1.0f - std::exp(-rate * dt);
    for (int i = 0; i < 3; i++) {
        value[i] += (goal[i] - value[i]) * t;
    }
}

float Wrap(float angle) {
    return std::remainder(angle, 2.0f * kPi);
}

} // namespace

namespace PlayAsLinkFishingCamera {

void Begin() {
    if (sCamera.active) {
        return;
    }
    sCamera.active = true;
    sCamera.haveRequest = false;
    // The follow camera's own orbit centre is where it looks; start the glide from there
    func_802C0490(sCamera.focus);
    sCamera.smoothValid = false;
    func_802914CC(PLAY_AS_LINK_FISHING_CAM_STATE);
}

void Set(const Request& request) {
    sCamera.request = request;
    sCamera.haveRequest = true;
}

void End() {
    if (!sCamera.active) {
        return;
    }
    sCamera = Camera();
    // The follow camera picks up from where this one is, and eases back to its place
    func_80291548();
    if (ncDynamicCamera_getState() == PLAY_AS_LINK_FISHING_CAM_STATE) {
        ncDynamicCamera_setState(0xB);
    }
}

} // namespace PlayAsLinkFishingCamera

extern "C" void port_playAsLinkFishingCamera_update(void) {
    if (!sCamera.active) {
        // Something left the state running without us (a stale state); hand it back
        func_80291548();
        ncDynamicCamera_setState(0xB);
        return;
    }
    if (!sCamera.haveRequest) {
        return;
    }
    const float dt = std::clamp(time_getDelta(), 0.0f, 0.1f);
    const PlayAsLinkFishingCamera::Request& request = sCamera.request;
    float eye[3];
    float focus[3];
    Goal(request, eye, focus);

    // The goal and the camera's present place, as angle, distance and height round Link
    const float goalAngle = std::atan2(eye[0] - request.link[0], eye[2] - request.link[2]);
    const float goalRadius = std::hypot(eye[0] - request.link[0], eye[2] - request.link[2]);
    const float goalHeight = eye[1] - request.link[1];
    if (!sCamera.smoothValid) {
        float camera[3];
        ncDynamicCamera_getPosition(camera);
        sCamera.angle = std::atan2(camera[0] - request.link[0], camera[2] - request.link[2]);
        sCamera.radius = std::hypot(camera[0] - request.link[0], camera[2] - request.link[2]);
        sCamera.height = camera[1] - request.link[1];
        sCamera.smoothValid = true;
    }
    const float t = 1.0f - std::exp(-kPositionRate * dt);
    sCamera.angle = Wrap(sCamera.angle + Wrap(goalAngle - sCamera.angle) * t);
    sCamera.radius += (goalRadius - sCamera.radius) * t;
    sCamera.height += (goalHeight - sCamera.height) * t;
    Approach(sCamera.focus, focus, kFocusRate, dt);

    float position[3] = { request.link[0] + std::sin(sCamera.angle) * sCamera.radius, request.link[1] + sCamera.height,
                          request.link[2] + std::cos(sCamera.angle) * sCamera.radius };
    ncDynamicCamera_setPosition(position);
    // Walls and the ground push the camera out of them, as they do for the follow camera
    func_802BE60C();
    float rotation[3];
    func_802BE6FC(rotation, sCamera.focus);
    ncDynamicCamera_setRotation(rotation);
}
