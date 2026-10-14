#pragma once
namespace shaderprebuild {
inline constexpr const char* setup_hint_for(bool android) {
    return android ? "Open the setup app and tap Start / resume / retry setup to rebuild shaders and pipelines." :
        "Run setup and choose Repair (or Update to this release) to rebuild shaders and pipelines.";
}
inline constexpr const char* setup_hint() {
#ifdef __ANDROID__
    return setup_hint_for(true);
#else
    return setup_hint_for(false);
#endif
}
}
