// Restarting the program (restart_switch.h).
#include "restart_switch.h"

#include <switch.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "../gfx/switch_renderer.h"
#include "../runtime.h"

namespace restart_switch {
namespace {
enum class Launch { Unknown, Forwarder, TitleOverride, Applet };

Launch launch() {
    static const Launch l = [] {
        if (appletGetAppletType() != AppletType_Application) return Launch::Applet;
        // the process's own RomFS (the title's, not the NRO's): the forwarder's names the NRO it loads
        if (R_FAILED(romfsMountFromCurrentProcess("wwhdtitle"))) return Launch::TitleOverride;
        struct stat st;
        const bool forwarder = stat("wwhdtitle:/nextNroPath", &st) == 0;
        romfsUnmount("wwhdtitle");
        return forwarder ? Launch::Forwarder : Launch::TitleOverride;
    }();
    return l;
}
}  // namespace

bool from_forwarder() { return launch() == Launch::Forwarder; }

uint64_t program_id() {
    u64 id = 0;
    if (R_FAILED(svcGetInfo(&id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0))) return 0;
    return id;
}

std::string launch_text() {
    char b[200];
    const unsigned long long id = (unsigned long long)program_id();
    switch (launch()) {
    case Launch::Forwarder:
        snprintf(b, sizeof b, "the HOME menu forwarder, title %016llx (reload works)", id);
        break;
    case Launch::TitleOverride:
        snprintf(b, sizeof b,
                 "application through another game's title %016llx (title override): no reload, it would start that "
                 "game; use the HOME menu forwarder",
                 id);
        break;
    default: snprintf(b, sizeof b, "applet (hbmenu): no reload, use quit and nxlink"); break;
    }
    return b;
}

bool restart(const char* arg) {
    if (!from_forwarder()) {
        LOG("[restart] not restarting: started as %s", launch_text().c_str());
        return false;
    }
    // the window first: a process ended by the restart with a present in flight or its layer open could take vi
    // down with it (2 of ~15 forwarder reloads on 2026-10-21; the new process then aborts in __nx_win_init)
    gfxsw::release_window(3000);
    LOG("[restart] the program restarts (appletRestartProgram, title %016llx)", (unsigned long long)program_id());
    log_flush();
    const Result rc = appletRestartProgram(arg, strlen(arg) + 1);
    // (on success the system ends this process)
    LOG("[restart] failed: appletRestartProgram rc 0x%x", rc);
    return false;
}
}  // namespace restart_switch
