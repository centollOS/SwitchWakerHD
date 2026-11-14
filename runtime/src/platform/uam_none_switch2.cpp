#include "uam_api.h"

namespace uam {

void init(bool) {}
void set_dual_issue(bool) {}
void shutdown() {}

Result compile(Stage, const char*) {
    Result r;
    r.log = "no runtime shader compiler is present on Switch 2";
    return r;
}

}  // namespace uam
