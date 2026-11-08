// gfx/vulkan/gpu_start_guard.h: a chosen GPU that cannot show the picture (issue #309) gets one new
// surface, then the automatic choice; no start repeats a failing choice.
#include "gfx/vulkan/gpu_start_guard.h"
#include <cassert>
using namespace gfxvk::gpu_guard;
int main() {
    // an ordinary start: the saved choice is used, nothing to tell
    auto p = plan_start({"uuid:nv", "", "", ""});
    assert(p.use == "uuid:nv" && !p.resetSetting && !p.clearFailed && !p.clearStarting && p.notice.empty());
    p = plan_start({"", "", "", ""});
    assert(p.use.empty() && !p.resetSetting && p.notice.empty());

    // the restart after the fallback: the setting is already automatic, the marker tells the player once
    p = plan_start({"", "", "uuid:nv", "uuid:nv"});
    assert(p.use.empty() && !p.resetSetting && p.clearFailed && p.notice == kNotice);
    // and the start after that is a normal one again
    p = plan_start({"", "", "", ""});
    assert(p.notice.empty() && !p.clearFailed);

    // the settings file could not be written: the relaunch still does not use the failed GPU
    p = plan_start({"uuid:nv", "", "", "uuid:nv"});
    assert(p.use.empty() && p.resetSetting && p.notice == kNotice);
    // the relaunch variable names another GPU (the player chose again meanwhile): the choice stands
    p = plan_start({"uuid:amd", "", "", "uuid:nv"});
    assert(p.use == "uuid:amd" && !p.resetSetting && p.notice.empty());

    // the last start with this choice ended before any picture (a driver crash): automatic, and say so
    p = plan_start({"uuid:nv", "uuid:nv", "", ""});
    assert(p.use.empty() && p.resetSetting && p.clearStarting && p.notice == kNoticeUnfinished);
    // a stale start marker of another choice is only cleared
    p = plan_start({"uuid:amd", "uuid:nv", "", ""});
    assert(p.use == "uuid:amd" && !p.resetSetting && p.clearStarting && p.notice.empty());
    // automatic with a leftover marker: nothing to reset
    p = plan_start({"", "uuid:nv", "", ""});
    assert(p.use.empty() && !p.resetSetting && p.clearStarting && p.notice.empty());

    // first picture: not armed (automatic or env choice) -> errors are reported as before
    FirstPicture g;
    assert(on_failure(g, 0, true, 0) == FailureAction::Report);
    g.armed = true;
    // not a surface error, or after a picture was shown -> as before
    assert(on_failure(g, 0, false, 0) == FailureAction::Report);
    assert(on_failure(g, 0, true, 1) == FailureAction::Report);
    // one new surface per screen, then the fallback, then nothing more
    assert(on_failure(g, 0, true, 0) == FailureAction::Retry);
    assert(on_failure(g, 1, true, 0) == FailureAction::Retry);
    assert(on_failure(g, 0, true, 0) == FailureAction::FallBack && g.fallingBack);
    assert(on_failure(g, 0, true, 0) == FailureAction::Wait);
    assert(on_failure(g, 1, false, 5) == FailureAction::Wait);
    FirstPicture h;
    h.armed = true;
    assert(on_failure(h, 7, true, 0) == FailureAction::Retry && h.retried[0]);
    return 0;
}
