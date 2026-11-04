// memory_trim.h: requests from any thread merge into the strongest until the render thread takes them.
#include "platform/memory_trim.h"

#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "memory_trim_test: %s\n", what); std::exit(1); }
}

int main() {
    int source = 0;
    check(memory_trim::take(source) == memory_trim::kNone, "nothing pending at start");
    memory_trim::request(memory_trim::kLight, 20);
    check(memory_trim::take(source) == memory_trim::kLight && source == 20, "a light request");
    check(memory_trim::take(source) == memory_trim::kNone, "taken once");
    memory_trim::request(memory_trim::kLight, 5);
    memory_trim::request(memory_trim::kFull, 15);
    memory_trim::request(memory_trim::kLight, 20);
    check(memory_trim::take(source) == memory_trim::kFull && source == 15, "the strongest wins, with its source");
    memory_trim::request(memory_trim::kFull, -1);
    memory_trim::request(memory_trim::kFull, 80);
    check(memory_trim::take(source) == memory_trim::kFull && source == 80, "equal levels: the latest source");
    std::puts("memory_trim_test: ok");
    return 0;
}
