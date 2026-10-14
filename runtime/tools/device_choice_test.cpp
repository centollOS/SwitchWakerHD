#include "gfx/vulkan/device_choice.h"
#include <cassert>
using namespace gfxvk;
int main() {
    DeviceInfo integrated{"Intel", "integrated", "uuid:a"}; integrated.core = true;
    DeviceInfo discrete{"NVIDIA", "discrete", "uuid:b"}; discrete.core = true;
    DeviceInfo old{"Old", "virtual", "id:1:2:Old"}; old.unavailable = "older than Vulkan 1.1";
    std::vector<DeviceInfo> d{integrated, discrete, old};
    auto c = choose_device(d, "", ""); assert(c.index == 0 && c.reason == "automatic: first usable");
    c = choose_device(d, "uuid:b", ""); assert(c.index == 1 && c.reason == "setting");
    c = choose_device(d, "missing", ""); assert(c.index == 0 && !c.fallback.empty());
    c = choose_device(d, old.identity, ""); assert(c.index == 0 && c.fallback.find(old.unavailable) != std::string::npos);
    c = choose_device(d, "uuid:a", "1"); assert(c.index == 1 && c.reason == "env");
    c = choose_device(d, "uuid:a", "nViDiA"); assert(c.index == 1 && c.reason == "env");
    for (auto env : {"99", "Old", "missing", "99999999999999999999999999999999"}) {
        c = choose_device(d, "uuid:b", env); assert(c.index == 0 && !c.fallback.empty());
    }
    std::swap(d[0], d[1]); c = choose_device(d, "uuid:b", ""); assert(c.index == 0 && c.reason == "setting");
    d[0].core = false; c = choose_device(d, "", ""); assert(c.index == 1);
    c = choose_device(d, "uuid:b", ""); assert(c.index == 0);
    c = choose_device(d, old.identity, "0"); assert(c.index == 0 && c.reason == "env");
    d[0].unavailable = d[1].unavailable = "cannot draw to the game window";
    assert(choose_device(d, "", "").index == -1);
    assert(choose_device({}, "", "").index == -1);
    DeviceInfo fallback{"fallback", "integrated", "uuid:rp"}; fallback.renderPasses = true;
    DeviceInfo khr{"KHR", "discrete", "uuid:khr"};
    assert(choose_device({fallback, khr, integrated}, "", "").index == 2);
    assert(choose_device({fallback, khr}, "", "").index == 1);
    assert(choose_device({fallback}, "", "").index == 0);
    assert(choose_device({fallback, khr}, "uuid:rp", "").index == 0);
}
