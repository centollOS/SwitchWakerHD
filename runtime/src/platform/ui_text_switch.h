// The fork's own on-screen texts (start-up screens, Prepare graphics) in the game's language: the "language" setting
// the game uses (Wii U code: 1 en, 2 fr, 3 de, 4 it, 5 es), else the console's language; English otherwise (the
// start-up screens' text console has no Japanese characters, nor the other scripts). The strings are UTF-8;
// to_console() turns them into the start-up text console's code page.
#pragma once
#include <string>

namespace ui_text {

struct Texts {
    // start-up: the first start
    const char *firstTitle, *firstNotPrepared, *firstExplain;
    const char *prepareNow, *prepareNowDetail, *playNow, *playNowDetail;
    // start-up: an update (Prepare graphics offered once)
    const char *updateTitle, *updateExplain, *prepareNowDetailUpdate, *later, *laterDetail;
    // start-up: the game's files missing (%s: the folder, then the missing file)
    const char *filesTitle, *filesNeed, *filesMissing, *filesCopy, *pressPlusToClose;
    // Prepare graphics: the loading screen
    const char *preparing, *placeOf, *starting, *minutesLeft, *workingOut, *cardBody, *stopping, *keepHolding,
        *holdToStop;
    // Prepare graphics: the settings menu's section
    const char *heading, *stopButton, *menuKept, *menuBody, *battery, *continueButton, *againButton, *prepareButton,
        *doneNote;
    // Prepare graphics: messages
    const char *ready, *statusLine, *etaPart, *ending, *endComplete, *endStopped, *endPlaceFailed, *endNoRestart,
        *errRunning, *errNoGame, *errCopy;
};

const Texts& tx();
// UTF-8 -> the libnx console's code page (437) for the start-up screens: accented Latin letters kept, others '?'
std::string to_console(const std::string& utf8);

}  // namespace ui_text
