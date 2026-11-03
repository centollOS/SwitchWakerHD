// (this fork, upstream sync bfd7dd6) the microphone backend (mic.h): none on the Switch, so the game's mic options
// see it as unavailable
#include "mic.h"

namespace mic {
void backend_start() { started(kUnavailable); }
void backend_stop() {}
}  // namespace mic
