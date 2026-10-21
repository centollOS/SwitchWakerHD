// The re-apply decision after a save-state load: the game's controller mode (as the state left it)
// against the host's choice, which a load never changes.
#include "controller_reapply.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using controller_reapply::needed_category;
int main() {
    // GamePad state (mode 1), now playing with the Pro Controller: switch the game to the Pro Controller
    static_assert(needed_category(1, true) == 0);
    // Off-TV GamePad play (mode 2) as well
    static_assert(needed_category(2, true) == 0);
    // Pro Controller state (mode 0), now playing with the GamePad: switch the game to the GamePad
    static_assert(needed_category(0, false) == 1);
    // same-controller loads: nothing changes
    static_assert(needed_category(0, true) == -1);
    static_assert(needed_category(1, false) == -1);
    static_assert(needed_category(2, false) == -1);
    // boot, title and file select (both), no manager yet (-1) or no controller (4): the game picks itself
    for (int mode : {-1, 3, 4, 5})
        for (bool pro : {false, true}) assert(needed_category(mode, pro) == -1);
    puts("controller re-apply decision: cross-controller loads switch the game, same-controller loads and title scenes do not");
}
