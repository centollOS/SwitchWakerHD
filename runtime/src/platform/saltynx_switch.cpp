// SaltyNX's FPS block for the console's overlays (saltynx_switch.h, docs/saltynx-plan.md).
//
// The interface, as SaltyNX 2.1.3 and Status Monitor 1.4.1 use it (written here from their behaviour, not their
// code):
// - the named port "SaltySD" takes one session at a time; requests are CMIF ("SFCI") with the process ID sent;
//   command 0 ends the session, 6 reserves bytes in the shared page (returns their offset), 7 returns the page's
//   handle (4 KiB);
// - a block starts with the magic 0x465053; an overlay takes the FIRST block with it in the page (4-byte steps),
//   clears pluginActive, waits 100 ms and calls the game running when it is set again: the block is written at
//   every frame. Reservations are never given back for a homebrew program, so a block left by an earlier start is
//   reused rather than reserving another (the page would fill up after ~20 starts, and an older block would hide
//   the new one);
// - FPSticks: the system ticks between the last 10 frames (a ring); FPS: frames in the last second; FPSavg: an
//   average; renderCalls/viewportCalls: when an overlay writes 0xFFFF to renderCalls[0].calls it wants the
//   resolutions, written at every frame from then on.
#include "saltynx_switch.h"

#include <switch.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "../runtime.h"

namespace saltynx {
namespace {

struct Res {
    uint16_t width, height, calls;
} __attribute__((packed));

// the block's layout (the fields the overlays read; SaltyNX 2.x appends GPU counters after expectedSetBuffers, 310
// bytes in all, reserved whole)
struct Block {
    uint32_t magic;
    uint8_t fps;
    float fpsAvg;
    bool pluginActive;
    uint8_t fpsLocked, fpsMode, zeroSync, patchApplied, api;
    uint32_t fpsTicks[10];
    uint8_t buffers, setBuffers, activeBuffers, setActiveBuffers, displaySync;
    Res renderCalls[8];
    Res viewportCalls[8];
    bool forceOriginalRefreshRate, dontForce60InDocked, forceSuspend;
    uint8_t currentRefreshRate;
    float readSpeedPerSecond;
    uint8_t fpsLockedDocked;
    uint64_t frameNumber;
    int8_t expectedSetBuffers;
} __attribute__((packed));
static_assert(sizeof(Block) == 174, "SaltyNX's NxFpsSharedBlock up to expectedSetBuffers");
constexpr uint32_t kMagic = 0x465053;
constexpr size_t kReserve = 310;  // SaltyNX 2.1.3's whole block
constexpr size_t kPage = 0x1000;

std::atomic<Block*> g_block{nullptr};
SharedMemory g_shm{};

// one request to SaltyNX: the process ID sent, a u64 in, a u64 out, a copied handle out (the ones asked for)
Result request(Service& s, uint32_t cmd, const uint64_t* in, uint64_t* out, Handle* handle) {
    if (handle)  // (no data back, only the handle)
        return serviceDispatch(&s, cmd, .in_send_pid = true, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                               .out_handles = handle);
    if (in && out) return serviceDispatchInOut(&s, cmd, *in, *out, .in_send_pid = true);
    return serviceDispatch(&s, cmd, .in_send_pid = true);
}

// the process has no session left (0x10801, console: the forwarder's application pool): one more, when the NPDM
// allows svcSetResourceLimitLimitValue (0x7E; calling an SVC it does not allow ends the process)
bool raise_session_limit() {
    if (!envIsSyscallHinted(0x7E)) return false;
    u64 value = 0;
    if (R_FAILED(svcGetInfo(&value, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !value) return false;
    const Handle limit = Handle(value);
    s64 max = 0, used = 0;
    bool ok = R_SUCCEEDED(svcGetResourceLimitLimitValue(&max, limit, LimitableResource_Sessions)) &&
              R_SUCCEEDED(svcGetResourceLimitCurrentValue(&used, limit, LimitableResource_Sessions));
    if (ok) {
        const Result rc = svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, max + 4);
        LOG("[saltynx] sessions %lld of %lld in use: limit raised to %lld (result 0x%X)", (long long)used,
            (long long)max, (long long)(max + 4), rc);
        ok = R_SUCCEEDED(rc);
    }
    svcCloseHandle(limit);
    return ok;
}

bool connect_once(Result& why) {
    Handle port;
    why = svcConnectToNamedPort(&port, "SaltySD");
    if (why == 0x10801 && raise_session_limit()) why = svcConnectToNamedPort(&port, "SaltySD");
    if (why == 0x10801) {  // else sm's session lent for a moment (NFSMW-NX's way)
        smExit();
        why = svcConnectToNamedPort(&port, "SaltySD");
        smInitialize();
        if (R_SUCCEEDED(why)) LOG("[saltynx] connected with sm's session lent");
    }
    if (R_FAILED(why)) return false;
    Service s;
    serviceCreate(&s, port);
    bool ok = false;
    do {
        Handle h = INVALID_HANDLE;
        if (R_FAILED(why = request(s, 7, nullptr, nullptr, &h))) break;  // the shared page's handle
        shmemLoadRemote(&g_shm, h, kPage, Perm_Rw);
        if (R_FAILED(why = shmemMap(&g_shm))) {
            shmemClose(&g_shm);
            break;
        }
        uint8_t* base = static_cast<uint8_t*>(shmemGetAddr(&g_shm));
        {  // (what the page holds: the overlays' blocks and SaltyNX's own words)
            uint32_t w[4];
            memcpy(w, base, sizeof w);
            int blocks = 0;
            for (size_t o = 0; o + 4 <= kPage; o += 4) {
                uint32_t m;
                memcpy(&m, base + o, 4);
                blocks += m == kMagic;
            }
            LOG("[saltynx] shared page: %08X %08X %08X %08X ..., %d block(s) with the magic", w[0], w[1], w[2], w[3], blocks);
        }
        // a block left by an earlier start (the first one is the one the overlays read)
        size_t at = kPage;
        for (size_t o = 0; o + sizeof(Block) <= kPage; o += 4) {
            uint32_t m;
            memcpy(&m, base + o, 4);
            if (m == kMagic) {
                at = o;
                break;
            }
        }
        if (at == kPage) {
            // 4 bytes more: the page's first word is SaltyNX's own (the display's refresh rate in its second byte,
            // console: a block at offset 0 lost its magic), so a block given offset 0 starts at 4
            uint64_t size = kReserve + 4, offset = 0;
            if (R_FAILED(why = request(s, 6, &size, &offset, nullptr)) || offset + kReserve + 4 > kPage) break;
            at = offset < 4 ? 4 : size_t(offset);
            memset(base + at, 0, kReserve);
            LOG("[saltynx] a new block at offset %zu of the shared page", at);
        } else {
            LOG("[saltynx] the block of an earlier start at offset %zu of the shared page", at);
        }
        Block* b = reinterpret_cast<Block*>(base + at);
        b->magic = kMagic;
        // 1 NVN, 2 EGL, 3 Vulkan: deko3d is NVN's level on the same GPU. Status Monitor shows the resolutions only for
        // 1 and above ("Game is not running or it's incompatible" with 0, console); the value is not shown
        b->api = 1;
        b->expectedSetBuffers = -1;
        g_block.store(b, std::memory_order_release);
        ok = true;
    } while (false);
    request(s, 0, nullptr, nullptr, nullptr);  // end the session: the port takes one at a time (the overlays')
    serviceClose(&s);
    return ok;
}

void thread_main(void*) {
    // SaltyNX may not be up yet at a cold boot: once a second for a minute
    Result why = 0;
    for (int tries = 0; tries < 60; tries++) {
        if (connect_once(why)) return;
        if (why == KERNELRESULT(NotFound)) {
            if (tries == 0) LOG("[saltynx] no SaltyNX (port not found): the overlays get no frame rate");
            return;
        }
        svcSleepThread(1'000'000'000);
    }
    LOG("[saltynx] could not reach SaltyNX (last result 0x%X): the overlays get no frame rate", why);
}

// render thread only
uint64_t g_lastTick = 0, g_secondStart = 0, g_avgTicks = 0;
uint32_t g_framesThisSecond = 0;
int g_ring = 0;
bool g_resolutions = false;

}  // namespace

void start() {
    if (const char* e = getenv("WWHD_SALTYNX"); e && *e && atoi(e) == 0) return;
    static Thread t;
    if (R_SUCCEEDED(threadCreate(&t, thread_main, nullptr, nullptr, 0x8000, 0x3B, -2))) threadStart(&t);
}

void frame(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) {
    Block* b = g_block.load(std::memory_order_acquire);
    if (!b) return;
    const uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    b->frameNumber = b->frameNumber + 1;
    if (g_lastTick) {
        const uint64_t delta = now - g_lastTick;
        b->fpsTicks[g_ring] = uint32_t(delta);
        g_ring = (g_ring + 1) % 10;
        g_avgTicks = g_avgTicks ? (9 * g_avgTicks + delta) / 10 : delta;
        b->fpsAvg = float(double(freq) / double(g_avgTicks));
    } else {
        for (int i = 0; i < 10; i++) b->fpsTicks[i] = uint32_t(freq / 30);  // (not 0: an overlay divides by their average)
        g_secondStart = now;
    }
    g_lastTick = now;
    g_framesThisSecond++;
    if (now - g_secondStart >= freq) {
        b->fps = uint8_t(g_framesThisSecond > 255 ? 255 : g_framesThisSecond);
        g_framesThisSecond = 0;
        g_secondStart = now - g_secondStart >= 2 * freq ? now : g_secondStart + freq;
    }
    if (!g_resolutions && b->renderCalls[0].calls == 0xFFFF) g_resolutions = true;  // an overlay asks for them
    if (g_resolutions) {
        Res r[8] = {};
        r[0] = {uint16_t(renderW), uint16_t(renderH), 1};
        memcpy(b->renderCalls, r, sizeof r);
        r[0] = {uint16_t(outputW), uint16_t(outputH), 1};
        memcpy(b->viewportCalls, r, sizeof r);
    }
    b->pluginActive = true;
}

}  // namespace saltynx
