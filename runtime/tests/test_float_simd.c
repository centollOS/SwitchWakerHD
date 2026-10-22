/* Host test (AArch64 only): the NEON float / paired-single helpers of ppc.h against the generic
 * integer path, bit for bit.   cc -O2 -Iruntime/include runtime/tests/test_float_simd.c -o /tmp/t && /tmp/t */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "ppc.h"

#ifndef PPC_NEON_FLOAT
int main(void) { puts("not AArch64: skipped"); return 0; }
#else
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static const uint32_t edge32[] = {0, 0x80000000u, 1, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00800000u, 0x3F800000u,
    0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7F800001u, 0xFF800001u, 0x7FA00000u, 0x7FBFFFFFu,
    0x7FC00000u, 0xFFC00000u, 0x7FC00001u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0xFFBFFFFFu};
static const uint64_t edge64[] = {0, 0x8000000000000000ull, 1, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
    0x3FF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF0000000000001ull, 0x7FF4000000000000ull, 0x7FF7FFFFFFFFFFFFull, 0x7FF8000000000000ull,
    0xFFF8000000000001ull, 0x7FFFFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull,
    0x47F0000000000000ull, 0x3690000000000000ull, 0x36A0000000000000ull, 0x3680000000000000ull,
    0xB690000000000001ull};
static uint32_t pick32(void) { return rnd32() % 4 == 0 ? edge32[rnd32() % (sizeof edge32 / 4)] : rnd32(); }
static uint64_t pick64(void) {
    uint32_t k = rnd32() % 4;
    if (k == 0) return edge64[rnd32() % (sizeof edge64 / 8)];
    if (k == 1) return f64_as_u64((double)u32_as_f32(pick32()));  /* exactly representable floats */
    return ((uint64_t)rnd32() << 32) | rnd32();
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) printf(__VA_ARGS__); } } while (0)

int main(void) {
    uint8_t* m = mmap(PPC_MEM_BASE, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (m != PPC_MEM_BASE) { puts("cannot map the guest window"); return 2; }
    long n = 0;
    for (int it = 0; it < 4000000; it++, n++) {
        uint32_t ea = 0x100 + rnd32() % 0x8000;  /* unaligned too */
        uint32_t a = pick32(), b = pick32();
        st32(ea, a); st32(ea + 4, b);
        /* loads */
        double r0 = (double)u32_as_f32(ld32(ea)), r1 = (double)u32_as_f32(ld32(ea + 4));
        double n0 = ldf32(ea);
        CHECK(f64_as_u64(n0) == f64_as_u64(r0), "ldf32 %08x: %016llx vs %016llx\n", a, (unsigned long long)f64_as_u64(n0), (unsigned long long)f64_as_u64(r0));
        double q0, q1;
        Cpu* c = NULL; (void)c;
        float64x2_t v = ldbe_f32x2(ea);
        q0 = vgetq_lane_f64(v, 0); q1 = vgetq_lane_f64(v, 1);
        CHECK(f64_as_u64(q0) == f64_as_u64(r0) && f64_as_u64(q1) == f64_as_u64(r1), "ldx2 %08x %08x\n", a, b);
        /* stores */
        uint64_t da = pick64(), db = pick64();
        uint32_t e0 = f32_as_u32((float)u64_as_f64(da)), e1 = f32_as_u32((float)u64_as_f64(db));
        stf32(ea + 16, u64_as_f64(da));
        CHECK(ld32(ea + 16) == e0, "stf32 %016llx: %08x vs %08x\n", (unsigned long long)da, ld32(ea + 16), e0);
        stbe_f32x2(ea + 32, u64_as_f64(da), u64_as_f64(db));
        CHECK(ld32(ea + 32) == e0 && ld32(ea + 36) == e1, "stx2 %016llx %016llx\n", (unsigned long long)da, (unsigned long long)db);
        st32(ea + 36, 0xDEADBEEFu);
        stbe_f32_raw(ea + 32, (float)u64_as_f64(da));
        CHECK(ld32(ea + 32) == e0 && ld32(ea + 36) == 0xDEADBEEFu, "st1 overran\n");
    }
    printf("%ld iterations, %d failures\n", n, fails);
    return fails != 0;
}
#endif
