// The fork's own on-screen texts (start-up screens, Prepare graphics, the settings menu's Switch tabs) in the game's language: the "language" setting
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
        *errRunning, *errNoGame, *errCopy, *waitingForGame;
    // the first start's loading card (background shaders)
    const char *bgTitle, *bgBody, *bgStarting, *bgReading, *bgCompiling, *bgSkip;
    // Prepare graphics: the menu section's text below its button (prepare_graphics_ui_switch.cpp)
    const char *pictureIntro;
};

const Texts& tx();

// The settings menu's Picture, Controls and Warp tabs and the Dev tab's first lines (overlay_switch.cpp), in the
// same language as tx(). A field a language leaves out is the English one.
struct MenuTexts {
    // Warp tab
    const char* warpWarning;
    const char* warpUnlock;
    const char* warpNowIn;
    const char* warpPending;
    const char* warpMain;
    const char* warpAll;
    const char* warpRoomPoint;
    // Picture tab: resolution
    const char* resolution;
    const char* handheld;
    const char* docked;
    const char* active;
    const char* modeHelp;
    const char* dynamicRes;
    const char* dynamicResHelp;
    const char* appliesDocked;
    const char* appliesHandheld;
    const char* resNow;
    // Picture tab: colours and effects
    const char* colours;
    const char* exposure;
    const char* exposureHelp;
    const char* contrast;
    const char* saturation;
    const char* gamma;
    const char* gammaHelp;
    const char* originalColours;
    const char* vivid;
    const char* anisoLabel;
    const char* anisoOffTip;
    const char* anisoTip;
    const char* shadowLabel;
    const char* shadowFast;
    const char* shadowOriginal;
    const char* shadowHigh;
    const char* shadowFastTip;
    const char* shadowOriginalTip;
    const char* shadowHighTip;
    const char* ao;
    const char* aoHelp;
    const char* fxaa;
    const char* fxaaHelp;
    const char* bloom;
    const char* bloomHelp;
    const char* blur;
    const char* blurHelp;
    // Picture tab: frame rate counter
    const char* fpsHeading;
    const char* fpsOff;
    const char* fpsRate;
    const char* fpsLoad;
    const char* fpsOffTip;
    const char* fpsRateTip;
    const char* fpsLoadTip;
    const char* fpsHelp;
    // Picture tab: experimental features
    const char* experimental;
    const char* experimentalWarn;
    const char* fps30;
    const char* fps30Tip;
    const char* fpsInterp;
    const char* fpsInterpTip;
    const char* fpsTrue60;
    const char* fpsTrue60Tip;
    const char* fpsModesHelp;
    const char* keepSpeed;
    const char* keepSpeedHelp;
    const char* drawnLately;
    // Controls tab
    const char* rumbleHeading;
    const char* rumble;
    const char* rumbleHelp;
    const char* gyroHeading;
    const char* gyroAim;
    const char* gyroAimHelp;
    const char* turnBy;
    const char* axisPlayer;
    const char* axisYaw;
    const char* axisRoll;
    const char* axisHelp;
    const char* sensX;
    const char* sensY;
    const char* invert;
    const char* defaultSens;
    const char* recalibrate;
    const char* recalibrateHelp;
    const char* gyroReceiving;
    const char* gyroCalibrating;
    const char* gyroNoMotion;
    const char* gyroNone;
    const char* gyroOff;
    const char* gyroStick;
    const char* menuHeading;
    const char* menuNote;
    // Dev tab: what a player sees before ticking the box
    const char* devWarn;
    const char* devShow;
    const char* devShowHelp;
    // Dev tab > Updates (update_notice_switch.h); updateNotice / updateNewer: printf with the new tag, then this build's
    const char* updateHeading;
    const char* updateCheck;
    const char* updateCheckHelp;
    const char* updateNotice;
    const char* updateNewer;
    const char* updateCurrent;   // %s this build's version
    const char* updateChecking;
    const char* updateFailed;    // %s why (English)
    const char* updateDevBuild;
    const char* updateOff;
};
const MenuTexts& menu();
// UTF-8 -> the libnx console's code page (437) for the start-up screens: accented Latin letters kept, others '?'
std::string to_console(const std::string& utf8);

}  // namespace ui_text
