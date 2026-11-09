#include "PlayAsLinkFishingCatch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <libultraship/libultraship.h>

#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Save/SaveManager.h"
#include "port/ShipInit.hpp"

#include "functions.h"
extern "C" {
#include "core1/mem.h"
#include "core2/gc/dialog.h"
}

namespace {

// The game's dialog boxes read their text from numbered assets. The catch needs words that no asset has, so the box
// opens on a plain one-page Bottles dialog (which gives it his portrait and the box's usual look) and its lines are
// swapped for the catch's as the box opens.
constexpr s32 kBaseDialog = ASSET_E12_DIALOG_BOTTLES_LEARNED_TUTORIAL_MOVES;
constexpr s32 kBottlesPortrait = 3;   // the dialog cmd for his portrait (sprite 0xC + 3)
constexpr s32 kCmdClose = -4;         // what ends a speaker's lines
constexpr s32 kCmdWaitForPlayer = -3; // holds the box open until A (or B) is pressed
constexpr s32 kCmdNextPage = -7;      // a break between two pages of the same speaker
constexpr s32 kDialogFlags = 0x6;     // like Bottles' own, without making the player look at him
constexpr int kPageCount = 2;
constexpr size_t kPageSize = 160;

// The lines are read by the box until it closes, so they live here
char sPages[kPageCount][kPageSize];
int sUsedPages = 0;

bool sRequested = false; // the box was asked for and its text has yet to be swapped in
bool sOpen = false;      // swapped in, and not yet dismissed

// The font only has capitals
std::string Upper(std::string text) {
    for (char& c : text) {
        c = (char)std::toupper((unsigned char)c);
    }
    return text;
}

bool StartsWithVowelSound(int number) {
    return number == 8 || number == 11 || number == 18 || (number >= 80 && number < 90);
}

std::string Plural(int count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// The pages of the box: what was caught, and what came out of it. The box breaks each page into lines and scrolls
// them by itself.
int BuildPages(const PlayAsLinkFishingCatch::Catch& caught) {
    int count = 0;
    auto add = [&](const std::string& text) {
        std::snprintf(sPages[count], kPageSize, "%s", Upper(text).c_str());
        count++;
    };
    add(std::string("You caught ") + (StartsWithVowelSound(caught.pounds) ? "an " : "a ") +
        std::to_string(caught.pounds) + "-pound " + (caught.loach ? "Hylian Loach!" : "Hylian Bass!") +
        (caught.newRecord ? " New record!" : ""));
    if (caught.eggs > 0 || caught.notes > 0) {
        std::string rewards;
        if (caught.eggs > 0) {
            rewards = Plural(caught.eggs, "egg", "eggs");
        }
        if (caught.notes > 0) {
            rewards += (rewards.empty() ? "" : " and ") + Plural(caught.notes, "note", "notes");
        }
        add(rewards + " popped out!");
    }
    return count;
}

// Takes the base dialog's lines out of the box that is opening, and puts the catch's in their place
void SwapText(OnDialogUpdate* ev) {
    // A page, a break between pages, and at the end a wait for the player and the close
    const int total = sUsedPages * 2 + 1;
    BKDialog* lines = (BKDialog*)bk_malloc(total * sizeof(BKDialog));
    int n = 0;
    for (int i = 0; i < sUsedPages; i++) {
        if (i > 0) {
            lines[n++] = { kCmdNextPage, (u8*)"" };
        }
        lines[n++] = { kBottlesPortrait, (u8*)sPages[i] };
    }
    lines[n++] = { kCmdWaitForPlayer, (u8*)"" };
    lines[n++] = { kCmdClose, (u8*)"" };

    // The box frees this array (but not what its lines point at) when it closes, so the old one goes now
    bk_free(ev->dialogs[0]);
    ev->dialogs[0] = lines;
    ev->stringCounts[0] = (u8)n;
    ev->strings[0] = (char*)lines[0].str;
}

void OnClosed(ActorMarker*, enum asset_e, s32) {
    sOpen = false;
}

void RegisterCatchDialog_Init() {
    REGISTER_LISTENER(OnDialogUpdate, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        auto* ev = reinterpret_cast<OnDialogUpdate*>(event);
        if (sRequested && ev->textId == kBaseDialog && ev->dialogs[0] != nullptr) {
            sRequested = false;
            sOpen = true;
            SwapText(ev);
        }
    });
}

} // namespace

namespace PlayAsLinkFishingCatch {

Records GetRecords() {
    Records records;
    const ShipSaveData* data = SaveManager_GetActiveShipData();
    if (data != nullptr) {
        records.valid = true;
        records.bestBass = data->fishingBestBass;
        records.bestLoach = data->fishingBestLoach;
        records.caught = (int)data->fishingCaught;
    }
    return records;
}

bool RecordCatch(int pounds, bool loach) {
    ShipSaveData* data = SaveManager_GetActiveShipData();
    if (data == nullptr) {
        return false;
    }
    pounds = std::clamp(pounds, 0, 0xFFFF);
    u16& best = loach ? data->fishingBestLoach : data->fishingBestBass;
    const bool beaten = pounds > best;
    if (beaten) {
        best = (u16)pounds;
    }
    if (data->fishingCaught < 0xFFFFFFFFu) {
        data->fishingCaught++;
    }
    return beaten;
}

bool ShowCatch(const Catch& caught) {
    if (sRequested || sOpen || gcdialog_hasCurrentTextId()) {
        return false;
    }
    sUsedPages = BuildPages(caught);
    sRequested = true;
    if (!gcdialog_showDialog(kBaseDialog, kDialogFlags, nullptr, nullptr, OnClosed, nullptr)) {
        sRequested = false;
        return false;
    }
    return true;
}

bool IsCatchShown() {
    // The box may also have been cleared without its callback (a map change, a cutscene)
    if (sOpen && gcdialog_getCurrentTextId() != kBaseDialog) {
        sOpen = false;
    }
    return sRequested || sOpen;
}

void Abandon() {
    sRequested = false;
    sOpen = false;
}

} // namespace PlayAsLinkFishingCatch

static RegisterShipInitFunc initCatchDialog(RegisterCatchDialog_Init);
