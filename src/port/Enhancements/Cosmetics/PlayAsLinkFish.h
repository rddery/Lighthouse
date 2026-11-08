#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_FISH_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_FISH_H

// The fish for Play as Link's fishing: Ocarina of Time's pond fish (and its Hylian Loach), swimming in whatever water
// the player fishes in. Like the rod, their models come from the player's own Ocarina of Time (USA 1.0) ROM at
// runtime (object_fish); this file and its .cpp hold the behaviour, which follows the Fishing actor in
// z_fishing.c: how fish wander, notice the lure, bite, get hooked and fight. Nothing from the ROM is stored here.
//
// Everything crossing this interface is in Banjo-Kazooie's units and the game's coordinates; the fish think in
// OoT's (2.6 times smaller) inside.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <ultra64.h>

namespace PlayAsLinkFish {

// Matrices Draw takes from the caller's pool for every fish it may draw
constexpr int kMaxFish = 10;
constexpr int kMatricesPerFish = 16;

// What the fish ask of the map and of the ripple effects the rod already has
class World {
public:
    virtual ~World() = default;
    // The water at (x, z): its surface, and the ground under it. `refY` is a height to search down from (a height
    // near the water). False if there is no water, or if the ground isn't below it.
    virtual bool WaterAt(float x, float z, float refY, float* surface, float* ground) = 0;
    virtual void Ripple(const float position[3], float scale, float target, float alpha) = 0;
};

// What the player does with the rod, each game frame
struct Frame {
    float dt = 0.0f;
    float player[3] = {};
    float playerYaw = 0.0f; // degrees, as the game has it
    float tip[3] = {};      // the rod's tip
    // The lure the way OoT's sRodCastState has it: 0 in hand or being reeled all the way in, 1 flying, 2 just landed,
    // 3 resting on the water where it can be reeled in
    int castState = 0;
    float lure[3] = {};
    float lureYaw = 0.0f;    // radians
    bool lureSplash = false; // the lure only just landed (D_80B7E114)
    int wiggle = 0;          // 1 for a gentle twitch of the lure, 2 for a strong one: this frame's only
    bool wiggling = false;   // the lure is still moving from a twitch (sLureWigglePosY > 1)
    bool a = false;          // A is held: reeling
    bool aPressed = false;   // A went down this frame
    bool bPressed = false;   // B went down this frame
    float stickX = 0.0f;     // -1 to 1
    float stickY = 0.0f;     // -1 (back) to 1
};

// What happened this frame
struct Result {
    int ticks = 0;        // 20 Hz game ticks the fish ran
    bool bit = false;     // a fish took the lure
    bool hookSet = false; // the player set the hook
    bool gotAway = false; // the fish let go or broke off
    bool missed = false;  // ... because the hook wasn't set in time
    bool caught = false;  // the fish was reeled in and is in the player's hands now
};

// The fish on the lure, if any (OoT's sRodCastState 4 and 5)
struct Fight {
    bool active = false;
    bool hooked = false;   // the hook is set (sLineHooked)
    bool caught = false;   // reeled in
    float mouth[3] = {};   // where the lure is: the fish's mouth
    float mouthYaw = 0.0f; // radians, the way the fish faces
    // The player's rod and body, from what the fish does (D_80B7A6BC target, sRodHitTimer, the wobble of the body)
    float bendTarget = 0.0f;
    bool pulling = false; // the fish is pulling the rod
    int hitTimer = 0;     // game ticks left of the hard tug that comes with setting the hook (sRodHitTimer)
    float length = 0.0f;  // the fish's length, in OoT's inches; a Hylian Loach is counted double for its weight
    bool loach = false;
    float faceYaw = 0.0f; // degrees: the way the player should look
    bool running = false;
};

class School {
public:
    School();
    ~School();
    School(const School&) = delete;
    School& operator=(const School&) = delete;

    struct Data;
    std::unique_ptr<Data> data;
};

// Turns the address of a display list in object_fish (0x06xxxxxx) into one the renderer can draw
using ListConverter = std::function<Gfx*(uint32_t address)>;

// Parses object_fish's fish and loach skeletons. `objectFish` has to outlive the result. Returns nullptr and sets
// `error` if the data isn't the USA 1.0 object.
std::unique_ptr<School> Build(const uint8_t* objectFish, size_t size, ListConverter convert, std::string& error);

// Fills the water in front of the player with fish: they go in the water around `center`, as many as it has room
// for. Returns how many there are.
int Spawn(School& school, World& world, const float center[3], const float player[3], float playerYaw);

// Takes every fish out of the water
void Clear(School& school);

bool HasFish(const School& school);

// One game frame
Result Update(School& school, World& world, const Frame& frame);

const Fight& GetFight(const School& school);

// The caught fish goes back into the water, thrown from the player's hands
void ReleaseCaught(School& school, World& world, const float player[3], float playerYaw);

// Draws the fish, in the world. `camera` is where the view is (the game's matrices are relative to it), `hand` is
// Link's right hand (where the caught fish is held) and `handYaw` where he faces, in degrees. To be called while the
// renderer is switched to the F3DEX2 microcode.
void Draw(School& school, Gfx** gfx, Mtx** mtx, const float camera[3], const MtxF& hand, float handYaw);

} // namespace PlayAsLinkFish

#endif
