// (this fork, issue #152) The console language a start gets when the player chose none: the Switch's system
// language (console_language.h). The settings menu's Language tab and WWHD_LANGUAGE still decide when set.
#include "console_language.h"

#include "game_languages.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

int system_language() {
    static const int lang = [] {
        int code = -1;
#ifdef __SWITCH__
        u64 lc = 0;
        SetLanguage l = SetLanguage_ENUS;
        if (R_SUCCEEDED(setInitialize())) {
            if (R_SUCCEEDED(setGetSystemLanguage(&lc)) && R_SUCCEEDED(setMakeLanguage(lc, &l))) {
                switch (l) {
                case SetLanguage_JA: code = 0; break;
                case SetLanguage_ENUS: case SetLanguage_ENGB: code = 1; break;
                case SetLanguage_FR: case SetLanguage_FRCA: code = 2; break;
                case SetLanguage_DE: code = 3; break;
                case SetLanguage_IT: code = 4; break;
                case SetLanguage_ES: case SetLanguage_ES419: code = 5; break;
                case SetLanguage_ZHCN: case SetLanguage_ZHHANS: code = 6; break;
                case SetLanguage_KO: code = 7; break;
                case SetLanguage_NL: code = 8; break;
                case SetLanguage_PT: case SetLanguage_PTBR: code = 9; break;
                case SetLanguage_RU: code = 10; break;
                case SetLanguage_ZHTW: case SetLanguage_ZHHANT: code = 11; break;
                default: code = 1; break;
                }
            }
            setExit();
        }
#endif
        return code;
    }();
    return lang;
}

int default_console_language() {
    const int sys = system_language();
    return sys < 0 ? 1 : game_lang::usable(sys);
}
