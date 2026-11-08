#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_H

// Fishing for Play as Link: Link takes out the fishing rod from Ocarina of Time's pond, casts it over the water in
// front of him and reels the lure back in. Like Link, the rod and lure come from the player's own Ocarina of Time
// (USA 1.0) ROM at runtime: this file and its .cpp only know where things are in object_fish and how OoT's
// Fishing actor and Player move them. Nothing from the ROM is stored here.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ultra64.h>

namespace PlayAsLinkFishing {

// object_fish's index in the OoT 1.0 (USA) file table (dmadata); the file is 0x155F0 bytes once decompressed.
constexpr int kFileIndex = 822;
constexpr size_t kFileSize = 0x155F0;

// The converted rod, lure and line models, and the fishing state. Built once from the ROM's object_fish, then
// updated and drawn from the game thread only.
class Fishing {
public:
    Fishing();
    ~Fishing();
    Fishing(const Fishing&) = delete;
    Fishing& operator=(const Fishing&) = delete;

    struct Data;
    std::unique_ptr<Data> data;
};

// Parses and converts object_fish (the decompressed file). `gameplayKeep` (segment 4, which the ripple's texture
// lives in) has to outlive the result. Returns nullptr and sets `error` if the data isn't the USA 1.0 object.
std::unique_ptr<Fishing> Build(std::vector<uint8_t> objectFish, const uint8_t* gameplayKeep, size_t gameplayKeepSize,
                               std::string& error);

// Link's body while he fishes. The animations are Link's own (gameplay_keep); the fishing code decides which one
// plays, and from which frame, because the throw is timed with the cast.
struct LinkPose {
    bool active = false;
    bool throwing = false; // else the waiting loop
    float throwFrame = 0.0f;
    // Stick tilt, from -1 to 1: OoT blends the reel animations in by how far the stick leans
    float leanX = 0.0f;
    float leanY = 0.0f;
    // A fish in his hands: holding it up (the catch animation, `catchFrame` frames in), then putting his arms down
    enum Catch { None, CatchHold, CatchEnd };
    Catch catching = None;
    float catchFrame = 0.0f;
    // How far his body tips forward with a fish's pull, in radians (negative is back)
    float tilt = 0.0f;
};
LinkPose GetLinkPose(const Fishing& fishing);

// Whether the rod is out.
bool IsActive(const Fishing& fishing);

// One game frame: D-pad Down takes the rod out (and puts the fish in the water) and puts it away, B casts, A reels.
// Pulling back on the stick or pressing A when a fish bites hooks it. Keeps Banjo from acting on the controller while
// the rod is out. `dt` is the game's frame time in seconds. Only call it in normal play.
void Update(Fishing& fishing, float dt);

// Puts the rod away at once (Play as Link turned off, the model replaced, the map changing...).
void Stop(Fishing& fishing);

// Matrices Draw takes from the caller's pool for the rod, lure and line (the fish take their own).
constexpr int kMatrixCount = 112;

// Draws the rod in Link's left hand, then the lure, the fish and the line in the world. `hand` and `rightHand` are
// the matrices of Link's hand limbs (modelview, with Banjo's model matrix and Link's scale in it); the right one is
// where a caught fish is held. To be called while the renderer is switched to the F3DEX2 microcode, with Link's
// lighting, so that the lists can be drawn as they are.
void Draw(Fishing& fishing, Gfx** gfx, Mtx** mtx, const MtxF& hand, const MtxF& rightHand);

} // namespace PlayAsLinkFishing

#endif
