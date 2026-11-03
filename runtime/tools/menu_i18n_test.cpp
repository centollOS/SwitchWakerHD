// (this fork) menu_i18n: the settings menu's texts in Spanish, Portuguese, Italian, German, French (runtime/src/menu_i18n.h).
#include <cassert>
#include <cstdio>
#include <cstring>

#include "menu_i18n.h"

static long portuguese() { return 9; }

int main() {
    using namespace menu_i18n;
    const char* saves = "Saves";
    assert(language() == kEnglish && tr(saves) == saves);  // English: the text itself
    set_language(5);
    assert(language() == kSpanish && strcmp(tr("Saves"), "Partidas") == 0);
    assert(strcmp(tr("Saves##tab"), "Partidas##tab") == 0);
    assert(strcmp(tr("Saves###saves"), "Partidas###saves") == 0);
    const char* again = tr("Saves##tab");
    assert(again == tr("Saves##tab"));  // the same pointer frame after frame
    const char* unknown = "Not in the table##x";
    assert(tr(unknown) == unknown);
    assert(tr(nullptr) == nullptr && strcmp(tr(""), "") == 0);
    set_language(1);
    assert(tr(saves) == saves);
    set_language_source(portuguese);  // read at the first text
    assert(strcmp(tr("Saves"), "Salvamentos") == 0 && language() == kPortuguese);
    set_language(4);
    assert(language() == kItalian && tr(saves) != saves);
    set_language(3);
    assert(language() == kGerman && tr(saves) != saves);
    assert(strncmp(tr("Saves##tab"), tr("Saves"), strlen(tr("Saves"))) == 0);
    set_language(2);
    assert(language() == kFrench && tr(saves) != saves);
    set_language(0);  // Japanese: the English menu
    assert(language() == kEnglish && tr(saves) == saves);
    int n = 0;
    const Row* r = rows(&n);
    assert(r && n > 0);
    for (int i = 0; i < n; i++) assert(r[i].en && r[i].es && r[i].pt && r[i].it && r[i].de && r[i].fr);
    printf("menu_i18n_test: OK (%d rows)\n", n);
    return 0;
}
