/*
 * The speaker PCM ring between SweetCodec (in mediaserverd: writes what VirtualAudio mixes for the codec's
 * output) and adspd (reads it into the ADSP's ASM stream). A file mapped MAP_SHARED by both: adspd creates it
 * (mode 0666), SweetCodec maps it at StartIO. 16-bit stereo at 48 kHz, the rate of the ASM stream.
 * Positions count frames since the ring was made; only the writer moves wpos, only the reader rpos.
 * MIT, as userland/.
 */
#ifndef SWEETPCM_H
#define SWEETPCM_H

#include <stdint.h>

#define SWEETPCM_PATH	"/var/local/pcm.ring"
#define SWEETPCM_MAGIC	0x4d435053u	/* "SPCM" */
#define SWEETPCM_RATE	48000u
#define SWEETPCM_FRAMES	16384u		/* 341 ms */

struct sweetpcm {
	uint32_t magic, rate, frames, channels;
	volatile uint64_t wpos;		/* SweetCodec */
	volatile uint64_t rpos;		/* adspd */
	uint32_t reserved[8];
	int16_t data[SWEETPCM_FRAMES * 2];
};

#define SWEETPCM_SIZE	sizeof(struct sweetpcm)

#endif
