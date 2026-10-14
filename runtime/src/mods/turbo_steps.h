// Quick doors (turbo.cpp): which parts of a logic step (fpcM_Management) an extra step runs, and a
// model of the game's process deletion against drawing that runtime/tools/turbo_steps_test.cpp
// checks (ctest turbo_steps).
//
// Why the extra steps leave process deletion out (issue #61): fopAcM_delete / fpcDt_ToDeleteQ puts a
// process into the delete queue with a timer of 1 (fpcDtTg_ToDeleteQ) and takes it off the draw
// list; each fpcDt_Handler counts the timer down and the one after that deletes the process
// (fpcDtTg_Do), so a process is deleted two frames after it was queued. The J3D packets its draw
// entered into a draw buffer (J3DDrawBuffer::entryImm: packet +0x94 = the buffer slot) stay listed
// until that buffer's next frameInit, in the next frame's draw, and the GPU may still read its
// display lists; the timer gives every packet a draw (and a frameInit) before the process heap
// is destroyed. When quick doors ran fpcDt_Handler in each extra step too, a process queued in a
// door event's step was deleted two extra steps later in the same frame, with its packets from the
// last draw still listed: its heap destroy reached ~J3DPacket with mEntryPtr set,
// "PROGRAM HALT J3DPacket.cpp:157 mEntryPtr == (0)" (Tingle's jail on Windfall, both door ways).
// The extra steps now skip fpcDt_Handler: deletion keeps counting frames, as in the game.
//
// The weather effects keep the same rule with a countdown of their own (issue #145): rain, stars,
// spores (housi), fog (moya) and waves each have a phase byte in g_env_light (dKyw_wether_move_draw
// 0257ACAC, enum WeatherPhase): 0 create, 1 move (the only phase whose packet is drawn: entered
// into a draw buffer), 2 drain, 3 delete (the packet is destroyed right away, delete_weather_packet,
// not through the delete queue). 1 -> 2 -> 3 -> delete takes three steps, so in the game the packet
// gets a draw (and its buffer a frameInit) between its last entry and its destruction. Leaving a
// Forbidden Woods spore room through a door (ky_tag0 sets the spore count only while Link stays in
// its room) runs that countdown during the door event; with three extra steps it finished within
// one frame and ~J3DPacket found the spore packet still listed: "PROGRAM HALT J3DPacket.cpp:157
// mEntryPtr == (0)". While any weather effect is draining or deleting, a frame gets no more extra
// steps, so the countdown counts drawn frames as in the game.
#pragma once

#include <cstdint>

namespace mods {

// parts of fpcM_Management one logic step runs (in the game's order)
struct StepParts {
    bool counter;   // cCt_Counter (end of the previous step)
    bool deletion;  // fpcDt_Handler: the delete queue (timers, deletes)
    bool priority;  // fpcPi_Handler
    bool creation;  // fpcCt_Handler
    bool execute;   // fpcEx_Handler: every process's execute
};

// the game's own step
constexpr StepParts kNormalStep{true, true, true, true, true};
// a quick-door extra step (turbo.cpp): everything but deletion, which stays once per frame
constexpr StepParts kDoorExtraStep{true, false, true, true, true};

// weather phases (enum WeatherPhase, d_kankyo_wether.h) and the phase bytes in g_env_light
// (10475A68) of the effects whose packet a phase-3 step destroys
enum WeatherPhase : uint8_t { kWeatherCreate, kWeatherMove, kWeatherDrain, kWeatherDelete };
struct WeatherEffect {
    const char* name;
    uint32_t phase_off;
};
constexpr WeatherEffect kWeatherEffects[] = {
    {"rain", 0xA3C},   // mbRainInitialized, packet +0xA44
    {"star", 0xA54},   // mbStarInitialized, packet +0xA60
    {"housi", 0xA70},  // mbHousiInitialized (spores), packet +0xA78
    {"moya", 0xA7C},   // mMoyaInitialized (fog), packet +0xA84
    {"wave", 0xA98},   // mWaveInitialized, packet +0xAA0
};
// an effect in this phase destroys its packet within the next two steps: they must be drawn frames
constexpr bool weather_tearing_down(uint8_t phase) { return phase == kWeatherDrain || phase == kWeatherDelete; }

}  // namespace mods
