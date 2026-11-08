#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_HORSE_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_HORSE_H

// Epona for Play as Link: when Banjo rides the Talon Trot, Link rides Epona. Like Link, she comes from the
// player's own Ocarina of Time (USA 1.0) ROM at runtime: this file and its .cpp only know where things are in
// object_horse and how to pose and draw a skinned (OoT "skin" limb) skeleton. Nothing from the ROM is stored here.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ultra64.h>

namespace PlayAsLinkHorse {

// object_horse's index in the OoT 1.0 (USA) file table (dmadata); the file is 0xDF10 bytes once decompressed.
constexpr int kFileIndex = 518;

// Matrices Draw takes from the caller's pool: the body's and one per rigid limb
constexpr int kMatrixCount = 11;

enum class Gait { Idle, Walk, Trot, Gallop, Jump };

// The converted model and the animation state. Built once from the ROM's object_horse, then drawn every frame
// from the game thread only.
class Epona {
public:
    Epona();
    ~Epona();
    Epona(const Epona&) = delete;
    Epona& operator=(const Epona&) = delete;

    struct Data;
    std::unique_ptr<Data> data;
};

// Parses and converts object_horse (the decompressed file). Returns nullptr and sets `error` if it isn't the
// USA 1.0 object.
std::unique_ptr<Epona> Build(std::vector<uint8_t> objectHorse, std::string& error);

// Advances the animation by dt seconds and draws Epona with `base` as her model matrix. Her model is in OoT
// units (she is about 5800 tall at the back and 8250 to the ears, like Link at about 6000; she faces +Z with the
// origin on the ground between her hooves), so `base` has to scale it down. `speed` scales playback (1 is how
// fast OoT's own gait plays at a typical speed); changing the gait blends for a moment. Writes the matrix for
// the rider's skeleton root to *rider: the saddle point OoT seats Link at, with `base` applied, so it can be
// used the way `base` is, to draw a rider modelled in OoT units. Takes kMatrixCount matrices from
// the caller's *mtx pool.
void Draw(Epona& epona, Gfx** gfx, Mtx** mtx, const MtxF& base, Gait gait, float speed, float dt, MtxF* rider);

} // namespace PlayAsLinkHorse

#endif
