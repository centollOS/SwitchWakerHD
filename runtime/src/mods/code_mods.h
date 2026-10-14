#pragma once
#include <string>
namespace mods::code {
struct Status {
    bool requested=false, target=false, building=false, ready=false;
    unsigned done=0,total=0;
    std::string stage,error,exe;
    std::string mod; // package queued to be enabled after the rebuild ("" for none)
};
bool enabled();
// fixed_build: this executable is never replaced by a cached code-mod build (the decomp version)
void startup(int argc, char** argv, bool fixed_build = false);
bool restart(std::string& error);
void request(bool on, const std::string& pending_mod = {}); // confirmation is shown in the Mods page
Status status();
void begin();
void cancel();
void dismiss();
}
