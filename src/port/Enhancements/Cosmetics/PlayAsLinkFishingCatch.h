#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_CATCH_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_CATCH_H

// What happens to the record books when Link lands a fish: the per-file records (kept with the save file's own port
// data) and the message in Banjo-Kazooie's dialog box that tells the player about the catch.

namespace PlayAsLinkFishingCatch {

// The current file's records. `valid` is false when no file is loaded (there is nowhere to keep them then).
struct Records {
    bool valid = false;
    int bestBass = 0;  // pounds, 0 if none caught
    int bestLoach = 0; // pounds, 0 if none caught
    int caught = 0;    // fish landed in all
};
Records GetRecords();

// Counts a catch of `pounds` against the current file and returns whether it beat the file's best of its kind (the
// first fish of a kind always does).
bool RecordCatch(int pounds, bool loach);

struct Catch {
    int pounds = 0;
    bool loach = false;
    bool newRecord = false;
    int eggs = 0; // what popped out of the fish
    int notes = 0;
};

// Opens the dialog box with the catch's text. Returns false if the game can't show a dialog right now (another one
// is open, a cutscene is playing...): the caller may try again.
bool ShowCatch(const Catch& caught);

// Whether the box opened by ShowCatch is still on screen. False once the player has dismissed it.
bool IsCatchShown();

// Forgets a box that was asked for (Play as Link or the rod switched off while it was up), so that a later dialog of
// the game's own can't be taken for it.
void Abandon();

} // namespace PlayAsLinkFishingCatch

#endif
