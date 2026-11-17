// The dr_flac implementation, with the options in dr_flac_config.h.
#include <stdint.h>

// dr_flac's 64-bit prediction (16-bit audio takes it too, as the side
// channel has 17 bits) multiplies as 64-bit numbers, a library call per
// tap on the Cortex-M0+. This gives the same result from 32-bit products:
// each sample splits into its signed high 16 bits and its low 16 bits.
// Coefficients have at most 15 bits (FLAC's limit, dr_flac refuses more),
// so every product fits in 32 bits. The low products add up in 64 bits and
// the high ones in 32, which holds while samples fit in 25 bits: up to
// 24-bit audio, the most FlacPlayer takes.
// int, as dr_flac's drflac_int32 (int32_t is long on arm-none-eabi).
int flacPredict64(unsigned int order, int shift, const int* coefficients, const int* samples);

#if defined(__ARM_ARCH_6M__) && defined(__thumb__)
// About 16 cycles a tap, from RAM (.time_critical), out of the way of
// core 0's flash reads. r0 order, r1 shift (kept in lr), r2 coefficients,
// r3 samples. r4 the high sum, r6:r5 the low sum.
__asm__(
    ".pushsection .time_critical.flacPredict64, \"ax\", %progbits\n"
    ".syntax unified\n"
    ".thumb\n"
    ".align 2\n"
    ".global flacPredict64\n"
    ".type flacPredict64, %function\n"
    ".thumb_func\n"
    "flacPredict64:\n"
    "push {r4-r7, lr}\n"
    "mov lr, r1\n"
    "movs r4, #0\n"
    "movs r5, #0\n"
    "movs r6, #0\n"
    "lsls r0, r0, #2\n"
    "beq 2f\n"
    "adds r0, r2, r0\n"
    "mov ip, r0\n"     // end of the coefficients
    "subs r3, #4\n"    // samples[-1]
    "1:\n"
    "ldr r0, [r3]\n"
    "subs r3, #4\n"
    "ldm r2!, {r7}\n"
    "asrs r1, r0, #16\n"
    "muls r1, r7, r1\n"
    "adds r4, r4, r1\n"
    "uxth r0, r0\n"
    "muls r0, r7, r0\n"
    "asrs r1, r0, #31\n"
    "adds r5, r5, r0\n"
    "adcs r6, r1\n"
    "cmp r2, ip\n"
    "bne 1b\n"
    "2:\n"
    // r1:r0 = high sum * 65536 + low sum, then >> shift (0 to 31).
    "lsls r0, r4, #16\n"
    "asrs r1, r4, #16\n"
    "adds r0, r0, r5\n"
    "adcs r1, r6\n"
    "mov r2, lr\n"
    "lsrs r0, r2\n"
    "movs r3, #32\n"
    "subs r3, r3, r2\n"
    "lsls r1, r3\n"    // a shift of 32 gives 0
    "orrs r0, r1\n"
    "pop {r4-r7, pc}\n"
    ".size flacPredict64, .-flacPredict64\n"
    ".popsection\n"
);
#else
// The same sums in C, for checking the method on the host.
int flacPredict64(unsigned int order, int shift, const int* coefficients, const int* samples) {
    unsigned int hi = 0;
    int64_t lo = 0;
    for (unsigned int i = 0; i < order; i++) {
        int x = samples[-1 - (int)i];
        int c = coefficients[i];
        hi += (unsigned int)(c * (x >> 16));
        lo += c * (x & 0xFFFF);
    }
    return (int)(((int64_t)(int)hi * 65536 + lo) >> shift);
}
#endif
#define DRFLAC_PREDICTION_64_S32 flacPredict64

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac_config.h"
