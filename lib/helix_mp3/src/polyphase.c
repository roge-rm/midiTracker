/* midiTracker: changed. With MP3_POLY_SPLIT (Cortex-M0+) PolyphaseMono()
 * and PolyphaseStereo() use xmp3_PolyDot() and the table from
 * xmp3_PolyInitTable() instead of 64-bit sums, see the end of this file. */
/* ***** BEGIN LICENSE BLOCK ***** 
 * Version: RCSL 1.0/RPSL 1.0 
 *  
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved. 
 *      
 * The contents of this file, and the files included with this file, are 
 * subject to the current version of the RealNetworks Public Source License 
 * Version 1.0 (the "RPSL") available at 
 * http://www.helixcommunity.org/content/rpsl unless you have licensed 
 * the file under the RealNetworks Community Source License Version 1.0 
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl, 
 * in which case the RCSL will apply. You may also obtain the license terms 
 * directly from RealNetworks.  You may not use this file except in 
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks 
 * applicable to this file, the RCSL.  Please see the applicable RPSL or 
 * RCSL for the rights, obligations and limitations governing use of the 
 * contents of the file.  
 *  
 * This file is part of the Helix DNA Technology. RealNetworks is the 
 * developer of the Original Code and owns the copyrights in the portions 
 * it created. 
 *  
 * This file, and the files included with this file, is distributed and made 
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER 
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES, 
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. 
 * 
 * Technology Compatibility Kit Test Suite(s) Location: 
 *    http://www.helixcommunity.org/content/tck 
 * 
 * Contributor(s): 
 *  
 * ***** END LICENSE BLOCK ***** */ 

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 * polyphase.c - final stage of subband transform (polyphase synthesis filter)
 *
 * This is the C reference version using __int64
 * Look in the appropriate subdirectories for optimized asm implementations 
 *   (e.g. arm/asmpoly.s)
 **************************************************************************************/

#include "coder.h"
#include "assembly.h"

/* input to Polyphase = Q(DQ_FRACBITS_OUT-2), gain 2 bits in convolution
 *  we also have the implicit bias of 2^15 to add back, so net fraction bits = 
 *    DQ_FRACBITS_OUT - 2 - 2 - 15
 *  (see comment on Dequantize() for more info)
 */
#define DEF_NFRACBITS	(DQ_FRACBITS_OUT - 2 - 2 - 15)	
#define CSHIFT	12	/* coefficients have 12 leading sign bits for early-terminating mulitplies */

static __inline short ClipToShort(int x, int fracBits)
{
	int sign;
	
	/* assumes you've already rounded (x += (1 << (fracBits-1))) */
	x >>= fracBits;
	
	/* Ken's trick: clips to [-32768, 32767] */
	sign = x >> 31;
	if (sign != (x >> 15))
		x = sign ^ ((1 << 15) - 1);

	return (short)x;
}

#ifndef MP3_POLY_SPLIT
#define MC0M(x)	{ \
	c1 = *coef;		coef++;		c2 = *coef;		coef++; \
	vLo = *(vb1+(x));			vHi = *(vb1+(23-(x))); \
	sum1L = MADD64(sum1L, vLo,  c1);	sum1L = MADD64(sum1L, vHi, -c2); \
}

#define MC1M(x)	{ \
	c1 = *coef;		coef++; \
	vLo = *(vb1+(x)); \
	sum1L = MADD64(sum1L, vLo,  c1); \
}

#define MC2M(x)	{ \
		c1 = *coef;		coef++;		c2 = *coef;		coef++; \
		vLo = *(vb1+(x));	vHi = *(vb1+(23-(x))); \
		sum1L = MADD64(sum1L, vLo,  c1);	sum2L = MADD64(sum2L, vLo,  c2); \
		sum1L = MADD64(sum1L, vHi, -c2);	sum2L = MADD64(sum2L, vHi,  c1); \
}

/**************************************************************************************
 * Function:    PolyphaseMono
 *
 * Description: filter one subband and produce 32 output PCM samples for one channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf 
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of one channel of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not supported
 *                (note max filter gain - see polyCoef[] comments)
 **************************************************************************************/
void PolyphaseMono(short *pcm, int *vbuf, const int *coefBase)
{	
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	Word64 sum1L, sum2L, rndVal;

	rndVal = (Word64)( 1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)) );

	/* special case, output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1L = rndVal;

	MC0M(0)
	MC0M(1)
	MC0M(2)
	MC0M(3)
	MC0M(4)
	MC0M(5)
	MC0M(6)
	MC0M(7)

	*(pcm + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);

	/* special case, output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64*16;
	sum1L = rndVal;

	MC1M(0)
	MC1M(1)
	MC1M(2)
	MC1M(3)
	MC1M(4)
	MC1M(5)
	MC1M(6)
	MC1M(7)

	*(pcm + 16) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);

	/* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples 31, 30, ... 17 */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm++;

	/* right now, the compiler creates bad asm from this... */
	for (i = 15; i > 0; i--) {
		sum1L = sum2L = rndVal;

		MC2M(0)
		MC2M(1)
		MC2M(2)
		MC2M(3)
		MC2M(4)
		MC2M(5)
		MC2M(6)
		MC2M(7)

		vb1 += 64;
		*(pcm)       = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*i) = ClipToShort((int)SAR64(sum2L, (32-CSHIFT)), DEF_NFRACBITS);
		pcm++;
	}
}

#define MC0S(x)	{ \
	c1 = *coef;		coef++;		c2 = *coef;		coef++; \
	vLo = *(vb1+(x));		vHi = *(vb1+(23-(x))); \
	sum1L = MADD64(sum1L, vLo,  c1);	sum1L = MADD64(sum1L, vHi, -c2); \
	vLo = *(vb1+32+(x));	vHi = *(vb1+32+(23-(x))); \
	sum1R = MADD64(sum1R, vLo,  c1);	sum1R = MADD64(sum1R, vHi, -c2); \
}

#define MC1S(x)	{ \
	c1 = *coef;		coef++; \
	vLo = *(vb1+(x)); \
	sum1L = MADD64(sum1L, vLo,  c1); \
	vLo = *(vb1+32+(x)); \
	sum1R = MADD64(sum1R, vLo,  c1); \
}

#define MC2S(x)	{ \
		c1 = *coef;		coef++;		c2 = *coef;		coef++; \
		vLo = *(vb1+(x));	vHi = *(vb1+(23-(x))); \
		sum1L = MADD64(sum1L, vLo,  c1);	sum2L = MADD64(sum2L, vLo,  c2); \
		sum1L = MADD64(sum1L, vHi, -c2);	sum2L = MADD64(sum2L, vHi,  c1); \
		vLo = *(vb1+32+(x));	vHi = *(vb1+32+(23-(x))); \
		sum1R = MADD64(sum1R, vLo,  c1);	sum2R = MADD64(sum2R, vLo,  c2); \
		sum1R = MADD64(sum1R, vHi, -c2);	sum2R = MADD64(sum2R, vHi,  c1); \
}

/**************************************************************************************
 * Function:    PolyphaseStereo
 *
 * Description: filter one subband and produce 32 output PCM samples for each channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf 
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of two channels of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * Notes:       interleaves PCM samples LRLRLR...
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not supported
 **************************************************************************************/
void PolyphaseStereo(short *pcm, int *vbuf, const int *coefBase)
{
	int i;
	const int *coef;
	int *vb1;
	int vLo, vHi, c1, c2;
	Word64 sum1L, sum2L, sum1R, sum2R, rndVal;

	rndVal = (Word64)( 1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)) );

	/* special case, output sample 0 */
	coef = coefBase;
	vb1 = vbuf;
	sum1L = sum1R = rndVal;

	MC0S(0)
	MC0S(1)
	MC0S(2)
	MC0S(3)
	MC0S(4)
	MC0S(5)
	MC0S(6)
	MC0S(7)

	*(pcm + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
	*(pcm + 1) = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);

	/* special case, output sample 16 */
	coef = coefBase + 256;
	vb1 = vbuf + 64*16;
	sum1L = sum1R = rndVal;

	MC1S(0)
	MC1S(1)
	MC1S(2)
	MC1S(3)
	MC1S(4)
	MC1S(5)
	MC1S(6)
	MC1S(7)

	*(pcm + 2*16 + 0) = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
	*(pcm + 2*16 + 1) = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);

	/* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples 31, 30, ... 17 */
	coef = coefBase + 16;
	vb1 = vbuf + 64;
	pcm += 2;

	/* right now, the compiler creates bad asm from this... */
	for (i = 15; i > 0; i--) {
		sum1L = sum2L = rndVal;
		sum1R = sum2R = rndVal;

		MC2S(0)
		MC2S(1)
		MC2S(2)
		MC2S(3)
		MC2S(4)
		MC2S(5)
		MC2S(6)
		MC2S(7)

		vb1 += 64;
		*(pcm + 0)         = ClipToShort((int)SAR64(sum1L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 1)         = ClipToShort((int)SAR64(sum1R, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*2*i + 0) = ClipToShort((int)SAR64(sum2L, (32-CSHIFT)), DEF_NFRACBITS);
		*(pcm + 2*2*i + 1) = ClipToShort((int)SAR64(sum2R, (32-CSHIFT)), DEF_NFRACBITS);
		pcm += 2;
	}
}
#endif /* !MP3_POLY_SPLIT */

#ifdef MP3_POLY_SPLIT
/* midiTracker: the filter for cores without a 32x32->64 multiply.
 *
 * Every output is a dot product of 16 vbuf values v with 16 coefficients
 * k, taken from v[0..7] and v[16..23] of the row. Each k fits in 20 bits
 * (CSHIFT above), so with v = vh*2^16 + vl (vl unsigned 16 bits) and
 * k = kh*2^10 + kl (kl unsigned 10 bits) every partial product fits in 26
 * bits and 16 of them sum in 32 bits without overflow:
 *   S = A26*2^26 + A16*2^16 + A10*2^10 + A0
 * with A26 = sum vh*kh, A16 = sum vh*kl, A10 = sum vl*kh, A0 = sum vl*kl.
 * The result is (int)((S + rnd) >> 20) of the 64-bit sum, exact to the bit.
 *
 * The table holds each k packed as (kh << 16) | kl, in the order the dot
 * product reads them, with the signs of the original MC0/MC2 terms folded
 * in: 16 words for output 0, 16 for output 16 (8 of them 0), then for each
 * of the 15 rows the 16 for output 16-i and the 16 for output 16+i. */

static int PolyPack(int k)
{
	return (int)(((unsigned int)(k >> 10) << 16) | (unsigned int)(k & 1023));
}

void xmp3_PolyInitTable(int *K, const int *c)
{
	int x, n;
	const int *b;

	for (x = 0; x < 8; x++) {
		K[x] = PolyPack(c[2*x]);
		K[15 - x] = PolyPack(-c[2*x + 1]);
		K[16 + x] = PolyPack(c[256 + x]);
		K[24 + x] = 0;
	}
	for (n = 0; n < 15; n++) {
		b = c + 16 + 16*n;
		for (x = 0; x < 8; x++) {
			K[32 + 32*n + x] = PolyPack(b[2*x]);
			K[32 + 32*n + 15 - x] = PolyPack(-b[2*x + 1]);
			K[48 + 32*n + x] = PolyPack(b[2*x + 1]);
			K[48 + 32*n + 15 - x] = PolyPack(b[2*x]);
		}
	}
}

/* out[j] = (int)((S + 2^25) >> 20) for the dot products of v with k + 16*j,
 * j < n (1 or more). 2^25 is the rounding the original adds, 1 << (DEF_NFRACBITS - 1 + 32 - CSHIFT). */
void xmp3_PolyDot(const int *v, const int *k, int *out, int n);

#if defined(__ARM_ARCH_6M__) && defined(__thumb__)
/* About 17 cycles a tap. Runs from RAM (.time_critical), it is the
 * decoder's inner loop. A0, A10, A16 and A26 live in r8, r9, r10 and r11. */
#define PD_TAP(voff, koff) \
	"ldr r2, [r0, #" #voff "]\n" \
	"ldr r4, [r1, #" #koff "]\n" \
	"asrs r3, r2, #16\n" \
	"uxth r2, r2\n" \
	"asrs r5, r4, #16\n" \
	"uxth r4, r4\n" \
	"movs r6, r2\n" \
	"muls r6, r4, r6\n" \
	"add r8, r6\n" \
	"muls r2, r5, r2\n" \
	"add r9, r2\n" \
	"muls r5, r3, r5\n" \
	"add r11, r5\n" \
	"muls r3, r4, r3\n" \
	"add r10, r3\n"

__asm__(
	".pushsection .time_critical.xmp3_PolyDot, \"ax\", %progbits\n"
	".syntax unified\n"
	".thumb\n"
	".align 2\n"
	".global xmp3_PolyDot\n"
	".type xmp3_PolyDot, %function\n"
	".thumb_func\n"
	"xmp3_PolyDot:\n"
	"push {r4-r7, lr}\n"
	"mov r4, r8\n"
	"mov r5, r9\n"
	"mov r6, r10\n"
	"mov r7, r11\n"
	"push {r4-r7}\n"
	"push {r2, r3}\n"           /* [sp] out, [sp + 4] n */
	"1:\n"
	"movs r2, #0\n"
	"mov r8, r2\n"
	"mov r9, r2\n"
	"mov r10, r2\n"
	"mov r11, r2\n"
	PD_TAP(0, 0) PD_TAP(4, 4) PD_TAP(8, 8) PD_TAP(12, 12)
	PD_TAP(16, 16) PD_TAP(20, 20) PD_TAP(24, 24) PD_TAP(28, 28)
	PD_TAP(64, 32) PD_TAP(68, 36) PD_TAP(72, 40) PD_TAP(76, 44)
	PD_TAP(80, 48) PD_TAP(84, 52) PD_TAP(88, 56) PD_TAP(92, 60)
	/* r7 = A26*64 + (A16 >> 4) + (A10 >> 10)
	 *    + ((((A16 & 15) << 16) + ((A10 & 1023) << 10) + A0 + 2^25) >> 20),
	 * the last sum is under 2^31. */
	"mov r2, r10\n"
	"lsls r3, r2, #28\n"
	"lsrs r3, r3, #12\n"
	"asrs r2, r2, #4\n"
	"mov r4, r9\n"
	"lsls r5, r4, #22\n"
	"lsrs r5, r5, #12\n"
	"asrs r4, r4, #10\n"
	"adds r3, r5\n"
	"mov r5, r8\n"
	"adds r3, r5\n"
	"movs r5, #1\n"
	"lsls r5, r5, #25\n"
	"adds r3, r5\n"
	"lsrs r3, r3, #20\n"
	"mov r7, r11\n"
	"lsls r7, r7, #6\n"
	"adds r7, r2\n"
	"adds r7, r4\n"
	"adds r7, r3\n"
	"ldr r6, [sp]\n"
	"stmia r6!, {r7}\n"
	"str r6, [sp]\n"
	"adds r1, #64\n"
	"ldr r6, [sp, #4]\n"
	"subs r6, #1\n"
	"str r6, [sp, #4]\n"
	"beq 2f\n"
	"b 1b\n"
	"2:\n"
	"add sp, #8\n"
	"pop {r4-r7}\n"
	"mov r8, r4\n"
	"mov r9, r5\n"
	"mov r10, r6\n"
	"mov r11, r7\n"
	"pop {r4-r7, pc}\n"
	".size xmp3_PolyDot, . - xmp3_PolyDot\n"
	".popsection\n"
);
#else
/* The same sums in C, for checking the method on the host. */
void xmp3_PolyDot(const int *v, const int *k, int *out, int n)
{
	int j, t, vv, kk, vh, kh, a10, a16, a26, r;
	unsigned int vl, kl, a0;

	for (j = 0; j < n; j++, k += 16) {
		a0 = 0; a10 = a16 = a26 = 0;
		for (t = 0; t < 16; t++) {
			vv = v[t < 8 ? t : t + 8];
			kk = k[t];
			vl = (unsigned int)vv & 0xFFFF; vh = vv >> 16;
			kl = (unsigned int)kk & 0xFFFF; kh = kk >> 16;
			a0 += vl * kl;
			a10 += (int)vl * kh;
			a16 += vh * (int)kl;
			a26 += vh * kh;
		}
		r = (int)(((((unsigned int)a16 & 15) << 16) + (((unsigned int)a10 & 1023) << 10) + a0 + (1u << 25)) >> 20);
		out[j] = (int)((unsigned int)a26 * 64u + (unsigned int)(a16 >> 4) + (unsigned int)(a10 >> 10) + (unsigned int)r);
	}
}
#endif

/* One channel's 32 samples, every stride'th short from pcm. In RAM on the
 * device, next to xmp3_PolyDot(). */
#if defined(__ARM_ARCH_6M__)
__attribute__((section(".time_critical.xmp3_PolyChannel"), noinline))
#endif
static void PolyChannel(short *pcm, int stride, int *vbuf, const int *K)
{
	int i, r[2];
	int *vb1;
	const int *k;

	xmp3_PolyDot(vbuf, K, r, 1);
	pcm[0] = ClipToShort(r[0], DEF_NFRACBITS);
	xmp3_PolyDot(vbuf + 64*16, K + 16, r, 1);
	pcm[16*stride] = ClipToShort(r[0], DEF_NFRACBITS);

	vb1 = vbuf + 64;
	k = K + 32;
	for (i = 15; i > 0; i--) {
		xmp3_PolyDot(vb1, k, r, 2);
		pcm[(16 - i)*stride] = ClipToShort(r[0], DEF_NFRACBITS);
		pcm[(16 + i)*stride] = ClipToShort(r[1], DEF_NFRACBITS);
		vb1 += 64;
		k += 32;
	}
}

/* coefBase is SubbandInfo.polyK here, see xmp3_PolyInitTable(). */
void PolyphaseMono(short *pcm, int *vbuf, const int *coefBase)
{
	PolyChannel(pcm, 1, vbuf, coefBase);
}

void PolyphaseStereo(short *pcm, int *vbuf, const int *coefBase)
{
	PolyChannel(pcm, 2, vbuf, coefBase);
	PolyChannel(pcm + 1, 2, vbuf + 32, coefBase);
}
#endif /* MP3_POLY_SPLIT */
