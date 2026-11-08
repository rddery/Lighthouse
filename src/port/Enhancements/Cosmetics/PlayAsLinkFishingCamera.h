#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_CAMERA_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_CAMERA_H

// The camera while Link fishes: it comes in close behind him for the cast, pulls back a little to keep him and the
// hooked fish in view, and swings round in front of him for the catch. It runs as a dynamic camera state of its own
// (the same way Free Look and Beak Bomb Aiming do), so the normal follow camera takes over again, with its own
// easing, the moment the state is left.

#define PLAY_AS_LINK_FISHING_CAM_STATE 0x17

#ifdef __cplusplus
extern "C" {
#endif
void port_playAsLinkFishingCamera_update(void);
#ifdef __cplusplus
}

namespace PlayAsLinkFishingCamera {

enum class View {
    Rod,   // over Link's shoulder, looking at the water he aims at
    Fight, // far enough back for both Link and the fish
    Catch  // three-quarter view from in front of Link, who holds the fish up
};

// What the camera has to frame this frame. `aim` is the lure (Rod) or the fish's mouth (Fight); Catch ignores it.
struct Request {
    View view = View::Rod;
    float link[3] = {}; // Link's feet
    float yaw = 0.0f;   // degrees, the way he faces
    float aim[3] = {};
};

// Takes the camera over (easing in from wherever it is)
void Begin();

// Where the camera should be heading this frame; call it every frame while the rod is out
void Set(const Request& request);

// Gives the camera back to the follow camera. Safe to call when it was never begun.
void End();

} // namespace PlayAsLinkFishingCamera
#endif

#endif
