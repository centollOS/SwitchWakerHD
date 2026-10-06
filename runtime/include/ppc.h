/* Espresso (Wii U PowerPC) CPU state and helpers used by recompiled code.
 *
 * Guest memory is a 4 GiB window mapped at a fixed host address, so a guest
 * effective address converts to a host pointer with a single add.
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __SWITCH__
/* Horizon has no fixed-address mappings and a 39-bit address space: the window is placed at
 * start-up (core.cpp). The asm read has no memory dependencies, so the compiler keeps the base in
 * a register across guest stores instead of reloading a global after each one. */
extern uint8_t* ppc_mem_base_var;
#if defined(PPC_BASE_REG) && !defined(__cplusplus)
/* WWHD_SWITCH_BASE_REG (experimental): the generated code reads the base from x28. Every file of the
 * program is compiled not to use x28 (-ffixed-x28); it is callee-saved, so the libraries keep it;
 * guest_call sets it before game code runs (core.cpp GuestBaseScope). The asm read below was repeated
 * in every basic block that touches guest memory (1,470 times in the hottest file). */
register uint8_t* ppc_mem_base_reg __asm__("x28");
static inline __attribute__((always_inline)) uint8_t* ppc_mem_base(void) { return ppc_mem_base_reg; }
#else
static inline __attribute__((always_inline)) uint8_t* ppc_mem_base(void) {
    uint8_t* p;
    __asm__("adrp %0, ppc_mem_base_var\n\tldr %0, [%0, :lo12:ppc_mem_base_var]" : "=r"(p));
    return p;
}
#endif
#define PPC_MEM_BASE (ppc_mem_base())
#else
#define PPC_MEM_BASE ((uint8_t*)0x200000000000ull)
#endif

typedef struct Cpu {
    uint32_t r[32];
    uint32_t lr, ctr;
    uint8_t cr[32];         /* one byte per CR bit; bit 4n+0 = crN.lt, +1 gt, +2 eq, +3 so */
    uint8_t xer_so, xer_ov, xer_ca;
    uint8_t xer_bc;
    /* the scheduler asks the thread holding this core to yield at its next function entry
     * (threads.cpp); in what was padding, so the layout (and save states) stay the same */
    volatile uint8_t preempt;
    /* the game's main thread (the runtime-call marker of the main-thread sampler reads it here: a
     * thread_local is a function call per access on the Switch) */
    uint8_t main_thread;
    uint8_t pad_[2];
    struct { double ps0, ps1; } f[32];
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t res_addr, res_val; /* lwarx/stwcx. reservation */
    uint32_t pc;               /* target for indirect dispatch */
    uint32_t core;             /* host-side: which emulated core this thread runs on */
    void* thread;              /* host-side: owning guest thread object */
} Cpu;

typedef void (*PpcFunc)(Cpu*);

/* runtime entry points */
void ppc_dispatch(Cpu* c);                       /* call/jump to c->pc */
/* Indirect call (bctrl/blrl) with a one-entry cache per call site: most are virtual calls that
 * always reach the same function, and the dispatch table (32 MB) costs a cache miss per lookup.
 * The entry packs the guest address (high half) and the host function as a 32-bit offset from
 * ppc_dispatch, so one 64-bit load reads a consistent pair; the table never changes once filled. */
void ppc_icall_miss(Cpu* c, uint64_t* slot);
/* Two entries per site, the latest target first: a virtual call site often alternates between a few
 * object types (one entry missed each time it changed). */
#define PPC_SITE_FN(e) ((PpcFunc)((uintptr_t)ppc_dispatch + (intptr_t)(int32_t)(uint32_t)(e)))
#define PPC_ICALL(c, t) do {                                                                       \
        static uint64_t ic_[2];                                                                    \
        uint64_t e_ = __atomic_load_n(&ic_[0], __ATOMIC_RELAXED);                                  \
        if (__builtin_expect((uint32_t)(e_ >> 32) == (t), 1))                                      \
            PPC_SITE_FN(e_)(c);                                                                    \
        else if ((uint32_t)((e_ = __atomic_load_n(&ic_[1], __ATOMIC_RELAXED)) >> 32) == (t))       \
            PPC_SITE_FN(e_)(c);                                                                    \
        else                                                                                       \
            ppc_icall_miss(c, ic_);                                                                \
    } while (0)
/* Indirect jumps (bctr: mostly virtual tail calls) remember their last target the same way:
 * PPC_IJUMP(c) is the function for c->pc, for "MUSTTAIL return PPC_IJUMP(c)(c);". Through
 * ppc_dispatch every one looked the target up in a table of 8 bytes per instruction of the game
 * (cache misses: ~1% of the main thread on the desktop). */
PpcFunc ppc_ijump_resolve(Cpu* c, uint64_t* slot);
#define PPC_IJUMP(c) ({                                                                            \
        static uint64_t ij_[2];                                                                    \
        uint64_t e_ = __atomic_load_n(&ij_[0], __ATOMIC_RELAXED), f_;                              \
        __builtin_expect((uint32_t)(e_ >> 32) == (c)->pc, 1) ? PPC_SITE_FN(e_)                     \
        : (uint32_t)((f_ = __atomic_load_n(&ij_[1], __ATOMIC_RELAXED)) >> 32) == (c)->pc           \
            ? PPC_SITE_FN(f_)                                                                      \
            : ppc_ijump_resolve((c), ij_);                                                         \
    })
void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn);
void ppc_trap(Cpu* c, uint32_t addr);
uint64_t ppc_timebase(void);
double ppc_fres(double x);
double ppc_frsqrte(double x);

#define MUSTTAIL __attribute__((musttail))

/* optional guest function-entry trace (runtime switch, see runtime/src/trace.cpp) */
extern int g_ppc_trace;
void ppc_trace_enter(uint32_t addr);
/* per-core scheduling: a higher-priority thread on this core is ready, yield at the next function entry */

void ppc_preempt(Cpu* c);
#ifdef __SWITCH__
/* no guest trace on the console: saves a load and a branch on every guest function call */
#define PPC_ENTER(a) do {                                                     \
        if (__builtin_expect(c->preempt, 0)) ppc_preempt(c);                  \
    } while (0)
#else
#define PPC_ENTER(a) do {                                                     \
        if (__builtin_expect(g_ppc_trace, 0)) ppc_trace_enter(a);             \
        if (__builtin_expect(c->preempt, 0)) ppc_preempt(c);                  \
    } while (0)
#endif

/* Callee-saved registers held in C locals across a call (tools/recomp/leaflocal.py
 * transform_nonleaf). The Switch trusts the PowerPC EABI: callees preserve r1, r14-r31 and f14-f31.
 * Desktop builds read them again (a save state may replace a thread's registers while it waits
 * inside a call). PPC_NONLEAF_CHECK (WWHD_RECOMP_NONLEAF_CHECK=1) aborts if a callee changed one. */
#if defined(PPC_NONLEAF_CHECK)
void ppc_keep_failed(Cpu* c, int reg, uint32_t fn);
#define PPC_KEEP_R(c, n, v, fn) do { if ((c)->r[n] != (v)) ppc_keep_failed(c, n, fn); } while (0)
#define PPC_KEEP_F(c, n, v0, v1, fn) do {                                                                 \
        double a_ = (c)->f[n].ps0, b_ = (c)->f[n].ps1, x_ = (v0), y_ = (v1);                             \
        if (memcmp(&a_, &x_, 8) || memcmp(&b_, &y_, 8)) ppc_keep_failed(c, 32 + (n), fn);              \
    } while (0)
#elif defined(__SWITCH__)
#define PPC_KEEP_R(c, n, v, fn) ((void)0)
#define PPC_KEEP_F(c, n, v0, v1, fn) ((void)0)
#else
#define PPC_KEEP_R(c, n, v, fn) ((v) = (c)->r[n])
#define PPC_KEEP_F(c, n, v0, v1, fn) ((v0) = (c)->f[n].ps0, (v1) = (c)->f[n].ps1)
#endif

/* the return address of a direct call to an ordinary game function (tools/recomp/recomp.py link):
 * the recompiled code returns with C returns, so the value matters only to guest code that reads
 * its saved copy on the stack (stack walkers: the watchdog's traces, desktop save states). The
 * Switch build leaves it out: one store, and building the constant, less per call. */
#if defined(__SWITCH__) || defined(PPC_ELIDE_LR)  /* PPC_ELIDE_LR: a desktop test of the Switch's form */
#define PPC_SET_LR(c, v) ((void)0)
#else
#define PPC_SET_LR(c, v) ((c)->lr = (v))
#endif

/* ---- memory ---- */
static inline uint8_t* ppc_ptr(uint32_t ea) { return PPC_MEM_BASE + ea; }
static inline uint8_t ld8(uint32_t ea) { return *ppc_ptr(ea); }
static inline uint16_t ld16(uint32_t ea) { uint16_t v; memcpy(&v, ppc_ptr(ea), 2); return __builtin_bswap16(v); }
static inline uint32_t ld32(uint32_t ea) { uint32_t v; memcpy(&v, ppc_ptr(ea), 4); return __builtin_bswap32(v); }
static inline uint64_t ld64(uint32_t ea) { uint64_t v; memcpy(&v, ppc_ptr(ea), 8); return __builtin_bswap64(v); }
static inline void st8(uint32_t ea, uint8_t v) { *ppc_ptr(ea) = v; }
static inline void st16(uint32_t ea, uint16_t v) { v = __builtin_bswap16(v); memcpy(ppc_ptr(ea), &v, 2); }
static inline void st32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(ppc_ptr(ea), &v, 4); }
static inline void st64(uint32_t ea, uint64_t v) { v = __builtin_bswap64(v); memcpy(ppc_ptr(ea), &v, 8); }

/* ---- bit casts ---- */
static inline double u64_as_f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t f64_as_u64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline float u32_as_f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f32_as_u32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static inline double ldf32(uint32_t ea) { return (double)u32_as_f32(ld32(ea)); }
static inline double ldf64(uint32_t ea) { return u64_as_f64(ld64(ea)); }
static inline void stf32(uint32_t ea, double d) { st32(ea, f32_as_u32((float)d)); }
static inline void stf64(uint32_t ea, double d) { st64(ea, f64_as_u64(d)); }

/* ---- integer helpers ---- */
static inline uint32_t rotl32(uint32_t v, uint32_t sh) { sh &= 31; return sh ? (v << sh) | (v >> (32 - sh)) : v; }

static inline void cr_set_s(Cpu* c, int f, int32_t a, int32_t b) {
    c->cr[4 * f + 0] = a < b; c->cr[4 * f + 1] = a > b; c->cr[4 * f + 2] = a == b; c->cr[4 * f + 3] = c->xer_so;
}
static inline void cr_set_u(Cpu* c, int f, uint32_t a, uint32_t b) {
    c->cr[4 * f + 0] = a < b; c->cr[4 * f + 1] = a > b; c->cr[4 * f + 2] = a == b; c->cr[4 * f + 3] = c->xer_so;
}
static inline void cr0_rc(Cpu* c, uint32_t v) { cr_set_s(c, 0, (int32_t)v, 0); }
/* The recompiler's condition-register liveness pass (tools/recomp/crlive.py) stores only the bits
 * that are read later: m has bit 0 = lt, 1 = gt, 2 = eq, 3 = so of the field. With PPC_CR_CHECK, the
 * dropped bits get a poison value that ppc_cr_read() refuses (a check build of the recompiler). */
#ifdef PPC_CR_CHECK
#define PPC_CR_DEAD(c, i) ((c)->cr[i] = 0x55)
void ppc_cr_poisoned(Cpu* c, int bit, uint32_t addr);
static inline uint8_t ppc_cr_read(Cpu* c, int bit, uint32_t addr) {
    if (__builtin_expect(c->cr[bit] == 0x55, 0)) ppc_cr_poisoned(c, bit, addr);
    return c->cr[bit];
}
#else
#define PPC_CR_DEAD(c, i) ((void)0)
#endif
static inline __attribute__((always_inline)) void cr_set_s_m(Cpu* c, int f, int32_t a, int32_t b, int m) {
    if (m & 1) c->cr[4 * f + 0] = a < b; else PPC_CR_DEAD(c, 4 * f + 0);
    if (m & 2) c->cr[4 * f + 1] = a > b; else PPC_CR_DEAD(c, 4 * f + 1);
    if (m & 4) c->cr[4 * f + 2] = a == b; else PPC_CR_DEAD(c, 4 * f + 2);
    if (m & 8) c->cr[4 * f + 3] = c->xer_so; else PPC_CR_DEAD(c, 4 * f + 3);
}
static inline __attribute__((always_inline)) void cr_set_u_m(Cpu* c, int f, uint32_t a, uint32_t b, int m) {
    if (m & 1) c->cr[4 * f + 0] = a < b; else PPC_CR_DEAD(c, 4 * f + 0);
    if (m & 2) c->cr[4 * f + 1] = a > b; else PPC_CR_DEAD(c, 4 * f + 1);
    if (m & 4) c->cr[4 * f + 2] = a == b; else PPC_CR_DEAD(c, 4 * f + 2);
    if (m & 8) c->cr[4 * f + 3] = c->xer_so; else PPC_CR_DEAD(c, 4 * f + 3);
}
static inline __attribute__((always_inline)) void cr0_rc_m(Cpu* c, uint32_t v, int m) { cr_set_s_m(c, 0, (int32_t)v, 0, m); }

static inline uint32_t ppc_divw(uint32_t a, uint32_t b) {
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return ((int32_t)a < 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

/* eight CR bytes (0/1, little-endian host) to eight bits, the first byte in the top bit */
static inline uint32_t ppc_cr_pack8(const uint8_t* p) {
    uint64_t x;
    memcpy(&x, p, 8);
    return (uint32_t)(((x & 0x0101010101010101ull) * 0x8040201008040201ull) >> 56);
}
static inline __attribute__((always_inline)) uint32_t ppc_mfcr(const Cpu* c) {
    return ppc_cr_pack8(c->cr) << 24 | ppc_cr_pack8(c->cr + 8) << 16 | ppc_cr_pack8(c->cr + 16) << 8 | ppc_cr_pack8(c->cr + 24);
}
static inline void ppc_mtcrf(Cpu* c, uint32_t crm, uint32_t v) {
    for (int f = 0; f < 8; f++)
        if (crm & (0x80u >> f))
            for (int b = 0; b < 4; b++) c->cr[4 * f + b] = (v >> (31 - (4 * f + b))) & 1;
}
static inline uint32_t ppc_mfxer(const Cpu* c) {
    return ((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc;
}
static inline void ppc_mtxer(Cpu* c, uint32_t v) {
    c->xer_so = (v >> 31) & 1; c->xer_ov = (v >> 30) & 1; c->xer_ca = (v >> 29) & 1; c->xer_bc = v & 0x7F;
}

/* lwarx / stwcx. */
static inline uint32_t ppc_lwarx(Cpu* c, uint32_t ea) {
    uint32_t raw = __atomic_load_n((uint32_t*)ppc_ptr(ea), __ATOMIC_SEQ_CST);
    c->res_addr = ea; c->res_val = raw;
    return __builtin_bswap32(raw);
}
static inline void ppc_stwcx(Cpu* c, uint32_t ea, uint32_t v) {
    int ok = 0;
    if (c->res_addr == ea) {
        uint32_t expected = c->res_val;
        ok = __atomic_compare_exchange_n((uint32_t*)ppc_ptr(ea), &expected, __builtin_bswap32(v), 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    c->res_addr = 0xFFFFFFFFu;
    c->cr[0] = 0; c->cr[1] = 0; c->cr[2] = (uint8_t)ok; c->cr[3] = c->xer_so;
}

static inline void ppc_dcbz(uint32_t ea) { memset(ppc_ptr(ea & ~31u), 0, 32); }

/* ---- floating point ---- */
static inline double round25(double d) {
    uint64_t v = f64_as_u64(d);
    v = (v & 0xFFFFFFFFF8000000ull) + (v & 0x8000000ull);
    return u64_as_f64(v);
}
static inline double to_single(double d) { return (double)(float)d; }
/* check builds (WWHD_RECOMP_SINGLE_CHECK=1): a multiplier operand the recompiler found to be single
 * precision, so round25 was left out (tools/recomp/ppc2c.py M), must come back unchanged from it */
void ppc_single_failed(uint32_t at, double v);
static inline double ppc_single_check(double v, uint32_t at) {
    if (f64_as_u64(round25(v)) != f64_as_u64(v)) ppc_single_failed(at, v);
    return v;
}

/* fcmpu / fcmpo. IEEE comparisons with a NaN are false, so lt/gt/eq need no NaN test; un is the
 * fourth outcome. FPSCR's FPCC field (which fcmp also sets) is not kept: only mffs and mcrfs read
 * it, and this game has neither (ppc2c warns when it translates one). */
static inline void cr_set_f(Cpu* c, int f, double a, double b) {
    c->cr[4 * f + 0] = a < b; c->cr[4 * f + 1] = a > b;
    c->cr[4 * f + 2] = a == b; c->cr[4 * f + 3] = (uint8_t)__builtin_isunordered(a, b);
}
/* cr_set_f storing only the bits in m (see cr_set_s_m) */
static inline __attribute__((always_inline)) void cr_set_f_m(Cpu* c, int f, double a, double b, int m) {
    if (m & 1) c->cr[4 * f + 0] = a < b; else PPC_CR_DEAD(c, 4 * f + 0);
    if (m & 2) c->cr[4 * f + 1] = a > b; else PPC_CR_DEAD(c, 4 * f + 1);
    if (m & 4) c->cr[4 * f + 2] = a == b; else PPC_CR_DEAD(c, 4 * f + 2);
    if (m & 8) c->cr[4 * f + 3] = (uint8_t)__builtin_isunordered(a, b); else PPC_CR_DEAD(c, 4 * f + 3);
}

/* The same compares for leaf functions that keep CR bits in C locals (tools/recomp/leaflocal.py):
 * lt, gt, eq, so point at the field's four locals. */
#ifdef PPC_CR_CHECK
#define PPC_CRL_DEAD(p) (*(p) = 0x55)
static inline uint8_t ppc_crl_read(Cpu* c, uint8_t v, int bit, uint32_t addr) {
    if (__builtin_expect(v == 0x55, 0)) ppc_cr_poisoned(c, bit, addr);
    return v;
}
#else
#define PPC_CRL_DEAD(p) ((void)0)
#endif
static inline __attribute__((always_inline)) void crl_set_s_m(uint8_t* lt, uint8_t* gt, uint8_t* eq, uint8_t* so,
                                                              const Cpu* c, int32_t a, int32_t b, int m) {
    if (m & 1) *lt = a < b; else PPC_CRL_DEAD(lt);
    if (m & 2) *gt = a > b; else PPC_CRL_DEAD(gt);
    if (m & 4) *eq = a == b; else PPC_CRL_DEAD(eq);
    if (m & 8) *so = c->xer_so; else PPC_CRL_DEAD(so);
}
static inline __attribute__((always_inline)) void crl_set_u_m(uint8_t* lt, uint8_t* gt, uint8_t* eq, uint8_t* so,
                                                              const Cpu* c, uint32_t a, uint32_t b, int m) {
    if (m & 1) *lt = a < b; else PPC_CRL_DEAD(lt);
    if (m & 2) *gt = a > b; else PPC_CRL_DEAD(gt);
    if (m & 4) *eq = a == b; else PPC_CRL_DEAD(eq);
    if (m & 8) *so = c->xer_so; else PPC_CRL_DEAD(so);
}
static inline __attribute__((always_inline)) void crl_set_f_m(uint8_t* lt, uint8_t* gt, uint8_t* eq, uint8_t* so,
                                                              const Cpu* c, double a, double b, int m) {
    (void)c;
    if (m & 1) *lt = a < b; else PPC_CRL_DEAD(lt);
    if (m & 2) *gt = a > b; else PPC_CRL_DEAD(gt);
    if (m & 4) *eq = a == b; else PPC_CRL_DEAD(eq);
    if (m & 8) *so = (uint8_t)__builtin_isunordered(a, b); else PPC_CRL_DEAD(so);
}
static inline __attribute__((always_inline)) void crl0_rc_m(uint8_t* lt, uint8_t* gt, uint8_t* eq, uint8_t* so,
                                                            const Cpu* c, uint32_t v, int m) {
    crl_set_s_m(lt, gt, eq, so, c, (int32_t)v, 0, m);
}
#define crl_set_s(lt, gt, eq, so, c, a, b) crl_set_s_m(lt, gt, eq, so, c, a, b, 15)
#define crl_set_u(lt, gt, eq, so, c, a, b) crl_set_u_m(lt, gt, eq, so, c, a, b, 15)
#define crl_set_f(lt, gt, eq, so, c, a, b) crl_set_f_m(lt, gt, eq, so, c, a, b, 15)
#define crl0_rc(lt, gt, eq, so, c, v) crl0_rc_m(lt, gt, eq, so, c, v, 15)

static inline uint64_t ppc_fctiwz(double d) {
    int32_t r;
    if (isnan(d)) r = (int32_t)0x80000000;
    else if (d >= 2147483647.0) r = 0x7FFFFFFF;
    else if (d <= -2147483648.0) r = (int32_t)0x80000000;
    else r = (int32_t)d;
    return 0xFFF8000000000000ull | (uint32_t)r;
}
static inline uint64_t ppc_fctiw(Cpu* c, double d) {
    switch (c->fpscr & 3) {
    case 0: d = nearbyint(d); break; /* default host mode is round-to-nearest-even */
    case 1: d = trunc(d); break;
    case 2: d = ceil(d); break;
    case 3: d = floor(d); break;
    }
    return ppc_fctiwz(d);
}
static inline double ppc_fsel(double a, double b, double cc) { return a >= 0.0 ? cc : b; }

/* ---- paired single quantization ---- */
/* 2^e for the 6-bit signed GQR scale, built from float bits (no libm call) */
static inline float psq_pow2(int e) { return u32_as_f32((uint32_t)(127 + e) << 23); }
static inline float psq_dequant(uint32_t data, uint32_t type, uint32_t scale) {
    if (type < 4) return u32_as_f32(data);  /* float: no scaling */
    float s = psq_pow2(-(int)((int32_t)(scale << 26) >> 26));
    switch (type) {
    case 4: return (float)(uint8_t)data * s;
    case 5: return (float)(uint16_t)data * s;
    case 6: return (float)(int8_t)data * s;
    case 7: return (float)(int16_t)data * s;
    default: return u32_as_f32(data);
    }
}
static inline uint32_t psq_quant(float v, uint32_t type, uint32_t scale) {
    if (type < 4) return f32_as_u32(v);
    float s = psq_pow2((int)((int32_t)(scale << 26) >> 26));
    switch (type) {
    case 4: v *= s; v = v < 0 ? 0 : v > 255 ? 255 : v; return (uint8_t)(uint32_t)v;
    case 5: v *= s; v = v < 0 ? 0 : v > 65535 ? 65535 : v; return (uint16_t)(uint32_t)v;
    case 6: v *= s; v = v < -128 ? -128 : v > 127 ? 127 : v; return (uint8_t)(int32_t)v;
    case 7: v *= s; v = v < -32768 ? -32768 : v > 32767 ? 32767 : v; return (uint16_t)(int32_t)v;
    default: return f32_as_u32(v);
    }
}
/* quantized formats (GQR type 4-7): out of line, so the float case below stays small and inline.
 * The _l forms take the two halves of the FPR separately (the recompiler keeps a leaf function's
 * registers in locals, tools/recomp/leaflocal.py). */
static __attribute__((noinline)) void psq_load_slow_l(Cpu* c, double* p0, double* p1, uint32_t ea, int w, int i) {
    uint32_t g = c->gqr[i], type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    uint32_t d0 = sz == 1 ? ld8(ea) : sz == 2 ? ld16(ea) : ld32(ea);
    *p0 = psq_dequant(d0, type, scale);
    if (w) *p1 = 1.0;
    else {
        uint32_t d1 = sz == 1 ? ld8(ea + 1) : sz == 2 ? ld16(ea + 2) : ld32(ea + 4);
        *p1 = psq_dequant(d1, type, scale);
    }
}
static __attribute__((noinline)) void psq_store_slow_l(Cpu* c, double v0, double v1, uint32_t ea, int w, int i) {
    uint32_t g = c->gqr[i], type = g & 7, scale = (g >> 8) & 0x3F;
    int sz = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
    uint32_t d0 = psq_quant((float)v0, type, scale);
    if (sz == 1) st8(ea, d0); else if (sz == 2) st16(ea, d0); else st32(ea, d0);
    if (!w) {
        uint32_t d1 = psq_quant((float)v1, type, scale);
        if (sz == 1) st8(ea + 1, d1); else if (sz == 2) st16(ea + 2, d1); else st32(ea + 4, d1);
    }
}
/* paired-single loads and stores: plain floats (GQR type 0-3) are nearly all of them. The generated
 * code (funcs.h) sets bit n of PPC_GQR_STATIC_FLOAT when the game never writes GQRn, which then stays
 * 0 (plain floats): with the constant GQR index of every call the check disappears (2,082 of the
 * game's 2,106 paired loads and stores use GQR0 or GQR1). */
#ifndef PPC_GQR_STATIC_FLOAT
#define PPC_GQR_STATIC_FLOAT 0
#endif
static inline __attribute__((always_inline)) void psq_load_l(Cpu* c, double* p0, double* p1, uint32_t ea, int w, int i) {
    if (((PPC_GQR_STATIC_FLOAT >> i) & 1) || __builtin_expect(((c->gqr[i] >> 16) & 7) < 4, 1)) {
        *p0 = u32_as_f32(ld32(ea));
        *p1 = w ? 1.0 : (double)u32_as_f32(ld32(ea + 4));
        return;
    }
    psq_load_slow_l(c, p0, p1, ea, w, i);
}
static inline __attribute__((always_inline)) void psq_store_l(Cpu* c, double v0, double v1, uint32_t ea, int w, int i) {
    if (((PPC_GQR_STATIC_FLOAT >> i) & 1) || __builtin_expect((c->gqr[i] & 7) < 4, 1)) {
        st32(ea, f32_as_u32((float)v0));
        if (!w) st32(ea + 4, f32_as_u32((float)v1));
        return;
    }
    psq_store_slow_l(c, v0, v1, ea, w, i);
}
static inline __attribute__((always_inline)) void psq_load(Cpu* c, int fd, uint32_t ea, int w, int i) {
    psq_load_l(c, &c->f[fd].ps0, &c->f[fd].ps1, ea, w, i);
}
static inline __attribute__((always_inline)) void psq_store(Cpu* c, int fs, uint32_t ea, int w, int i) {
    psq_store_l(c, c->f[fs].ps0, c->f[fs].ps1, ea, w, i);
}

#ifdef __cplusplus
}
#endif
