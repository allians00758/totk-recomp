// Build: clang -O1 -march=x86-64-v3 -Iruntime tools/test_excl_pair.c -pthread -o /tmp/t && /tmp/t
// LDAXP/STLXP as lifted (tools/lifter.py): a 128-bit atomic store loop, from several threads at once, and the
// failure paths (another address, no ldxp first, the value changed in between).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

static __uint128_t cell __attribute__((aligned(16)));
static uint64_t counter[2] __attribute__((aligned(16)));

// the guest's 128-bit atomic increment of both halves: ldaxp x9, x8, [x20]; add; stlxp w10, x9, x8, [x20]; cbnz w10
static void *worker(void *arg) {
    Cpu c = {0};
    for (int i = 0; i < 200000; i++) {
        c.x[20] = (uint64_t)(uintptr_t)counter;
        for (;;) {
            { __uint128_t p_ = ldxp128(&c, c.x[20]); c.x[9] = (uint64_t)p_; c.x[8] = (uint64_t)(p_ >> 64); }
            c.x[9] += 1, c.x[8] += 3;
            { uint32_t st = stxp128(&c, c.x[20], c.x[9], c.x[8]); c.x[10] = (uint32_t)(st); }
            if (!c.x[10]) break;
        }
    }
    return arg;
}

int main(void) {
    int bad = 0;
    Cpu c = {0};
    cell = ((__uint128_t)0x1111222233334444ull << 64) | 0x5555666677778888ull;
    __uint128_t p = ldxp128(&c, (uintptr_t)&cell);
    if ((uint64_t)p != 0x5555666677778888ull || (uint64_t)(p >> 64) != 0x1111222233334444ull) bad |= 1;
    if (stxp128(&c, (uintptr_t)&cell, 1, 2) != 0) bad |= 2;                       // succeeds
    if ((uint64_t)cell != 1 || (uint64_t)(cell >> 64) != 2) bad |= 4;               // rt low, rt2 high
    if (stxp128(&c, (uintptr_t)&cell, 5, 6) != 1) bad |= 8;                         // monitor cleared: fails
    ldxp128(&c, (uintptr_t)&cell);
    cell = 99;                                                                      // changed in between: fails
    if (stxp128(&c, (uintptr_t)&cell, 5, 6) != 1 || cell != 99) bad |= 16;
    ldxp128(&c, (uintptr_t)&cell);
    if (stxp128(&c, (uintptr_t)counter, 5, 6) != 1) bad |= 32;                      // another address: fails

    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    if (counter[0] != 800000 || counter[1] != 2400000) bad |= 64;
    printf("counter %llu %llu, %s (%d)\n", (unsigned long long)counter[0], (unsigned long long)counter[1],
           bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
