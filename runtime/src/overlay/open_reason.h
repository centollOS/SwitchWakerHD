#pragma once
#include "overlay.h"
#include <string>
namespace overlay {
inline const char* reason_name(OpenReason r) {
    switch (r) {
    case OpenReason::F1: return "F1 key"; case OpenReason::Esc: return "Esc key";
    case OpenReason::Home: return "Home"; case OpenReason::View: return "View";
    case OpenReason::B: return "B"; case OpenReason::CloseButton: return "close button";
    case OpenReason::LoadSlot: return "load slot"; case OpenReason::Focus: return "focus";
    default: return "API";
    }
}
inline std::string open_message(bool open, OpenReason reason, KeyOrigin origin, uint64_t frame) {
    return std::string("[overlay] ") + (open ? "opened (" : "closed (") + reason_name(reason) +
        ", window " + origin.name + " id " + std::to_string(origin.window) +
        ", timestamp " + std::to_string(origin.timestamp) + ", frame " + std::to_string(frame) + ")";
}
}
