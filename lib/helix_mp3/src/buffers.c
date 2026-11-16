/* midiTracker replacement for Helix's buffers.c: the decoder's state is
 * carved from one caller-owned block (MP3SetPool()) instead of the heap.
 * MP3FreeDecoder() only forgets it, the caller owns the memory. */

#include <string.h>
#include "coder.h"

static unsigned char *poolMem;
static int poolBytes;

#define ALIGN4(n) (((n) + 3) & ~3)

int MP3PoolBytes(void)
{
	return ALIGN4(sizeof(MP3DecInfo)) + ALIGN4(sizeof(FrameHeader)) + ALIGN4(sizeof(SideInfo)) +
	       ALIGN4(sizeof(ScaleFactorInfo)) + ALIGN4(sizeof(HuffmanInfo)) + ALIGN4(sizeof(DequantInfo)) +
	       ALIGN4(sizeof(IMDCTInfo)) + ALIGN4(sizeof(SubbandInfo));
}

void MP3SetPool(void *mem, int nBytes)
{
	poolMem = (unsigned char *)mem;
	poolBytes = nBytes;
}

MP3DecInfo *AllocateBuffers(void)
{
	MP3DecInfo *d;
	unsigned char *p = poolMem;

	if (!p || poolBytes < MP3PoolBytes())
		return 0;
	/* DSP primitives assume their state starts at 0 */
	memset(p, 0, MP3PoolBytes());

	d = (MP3DecInfo *)p;                p += ALIGN4(sizeof(MP3DecInfo));
	d->FrameHeaderPS = p;               p += ALIGN4(sizeof(FrameHeader));
	d->SideInfoPS = p;                  p += ALIGN4(sizeof(SideInfo));
	d->ScaleFactorInfoPS = p;           p += ALIGN4(sizeof(ScaleFactorInfo));
	d->HuffmanInfoPS = p;               p += ALIGN4(sizeof(HuffmanInfo));
	d->DequantInfoPS = p;               p += ALIGN4(sizeof(DequantInfo));
	d->IMDCTInfoPS = p;                 p += ALIGN4(sizeof(IMDCTInfo));
	d->SubbandInfoPS = p;
	return d;
}

void FreeBuffers(MP3DecInfo *mp3DecInfo)
{
	(void)mp3DecInfo;
}
