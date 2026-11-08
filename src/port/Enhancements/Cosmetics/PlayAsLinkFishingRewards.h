#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_REWARDS_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_FISHING_REWARDS_H

// What a catch earns Link: pickups that pop out around him when he holds the fish up, for him to walk into.
// Eggs for every fish; a trophy fish also frees notes.

namespace PlayAsLinkFishingRewards {

// Eggs for a fish of this weight in pounds: about one per two pounds, from 1 to 10
int EggsFor(int pounds);

// Notes for a trophy fish (10 pounds and up), and for any Hylian Loach; none for a lighter fish. Randomizer files
// never get notes.
int NotesFor(int pounds, bool loach);

struct Given {
    int eggs = 0;
    int notes = 0;
};

// Pops the rewards out from Link's chest, scattering them on the side away from the water he faces (`yaw`, in
// degrees). A note is one of the map's own uncollected ones, so the level can never hold more than its 100; where
// there are none left (or the level has none), eggs come out instead.
Given Give(int pounds, bool loach, const float link[3], float yaw);

} // namespace PlayAsLinkFishingRewards

#endif
