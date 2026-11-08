#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_H

// Play as Link: draws adult Link from the player's own Ocarina of Time (USA 1.0) ROM in place of Banjo and
// Kazooie. Nothing from that ROM ships with Lighthouse: the player picks it once, and the few files Link needs
// are kept in "oot_link.bin" next to the saves.

#include <ultra64.h>

#include "port/UI/cvar_prefixes.h"

#define CVAR_PLAY_AS_LINK CVAR_COSMETIC("Player.PlayAsLink")

#ifdef __cplusplus
#include <string>

namespace PlayAsLink {

// Reads Link's model and animations from an Ocarina of Time ROM and keeps them for later sessions. Returns an
// error message, or an empty string on success.
std::string ImportRom(const std::string& romPath);
// Whether Link's data was imported (and could be read).
bool IsAvailable();
// Whether Epona is there too (data imported by an older version doesn't have her).
bool HasEpona();
// Whether the fishing rod is there too (data imported by an older version doesn't have it).
bool HasFishing();
// Opens a file picker for the ROM, then imports it.
void PickRom();
// The result of the last import, for the menu.
const std::string& GetStatus();

} // namespace PlayAsLink

extern "C" {
#endif

// Whether Link replaces the player model this frame: the option is on, Link's data is there, and the player is
// Banjo and Kazooie (not a transformation).
s32 PlayAsLink_shouldDraw(s32 modelId);

// A scratch display list for drawing Banjo with nothing shown, which keeps his draw's side effects (Kazooie's
// egg and beak positions, the marker's visibility).
Gfx* PlayAsLink_getScratchGfx(void);

// Draws Link with the model matrix Banjo was just drawn with. kazooieFacing: Banjo's model is turned to face
// Kazooie's way (the Talon Trot and the wading boots).
void PlayAsLink_draw(Gfx** gfx, Mtx** mtx, const MtxF* modelMatrix, s32 kazooieFacing);

#ifdef __cplusplus
}
#endif

#endif
