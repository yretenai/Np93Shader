/*
 * SPDX-FileCopyrightText: 2025 Austin Appleby, Peter Scott, Np93-237
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>

#ifdef __GNUC__
#define FORCE_INLINE __attribute__((always_inline)) inline
#else
#define FORCE_INLINE inline
#endif

#define ROTL64(x, y) rotl64(x, y)
#define getblock(p, i) (p[i])

// https://mostlymangling.blogspot.com/2019/12/stronger-better-morer-moremur-better.html
uint64_t moremur(uint64_t x) {
	x ^= x >> 27;
	x *= 0x3C79AC492BA7B653UL;
	x ^= x >> 33;
	x *= 0x1C69B3F74AC4AE35UL;
	x ^= x >> 27;

	return x;
}

static FORCE_INLINE uint64_t rotl64(uint64_t x, int8_t r) {
	return (x << r) | (x >> (64 - r));
}

// modified murmurhash
// https://github.com/PeterScott/murmur3/blob/master/murmur3.c
uint64_t CreateHash(const void *key, const uint32_t len) {
	const uint8_t *data = (const uint8_t *)key;
	const int nblocks = len / 16;
	int i;

	uint64_t h1 = 0x42F0E1EBA9EA3693ull;
	uint64_t h2 = 0x42F0E1EBA9EA3693ull;

	uint64_t c1 = 0x87c37b91114253d5ull;
	uint64_t c2 = 0x4cf5ad432745937full;

	//----------
	// body

	const uint64_t *blocks = (const uint64_t *)(data);

	for (i = 0; i < nblocks; i++) {
		uint64_t k1 = getblock(blocks, i * 2 + 0);
		uint64_t k2 = getblock(blocks, i * 2 + 1);

		k1 *= c1;
		k1 = ROTL64(k1, 31);
		k1 *= c2;
		h1 ^= k1;

		h1 = ROTL64(h1, 27);
		h1 += h2;
		h1 = h1 * 5 + 0x52dce729;

		k2 *= c2;
		k2 = ROTL64(k2, 33);
		k2 *= c1;
		h2 ^= k2;

		h2 = ROTL64(h2, 31);
		h2 += h1;
		h2 = h2 * 5 + 0x38495ab5;
	}

	//----------
	// tail

	const uint8_t *tail = (const uint8_t *)(data + nblocks * 16);

	uint64_t k1 = 0;
	uint64_t k2 = 0;

	switch (len & 15) {
	case 15:
		k2 ^= (uint64_t)(tail[14]) << 48;
		[[fallthrough]];
	case 14:
		k2 ^= (uint64_t)(tail[13]) << 40;
		[[fallthrough]];
	case 13:
		k2 ^= (uint64_t)(tail[12]) << 32;
		[[fallthrough]];
	case 12:
		k2 ^= (uint64_t)(tail[11]) << 24;
		[[fallthrough]];
	case 11:
		k2 ^= (uint64_t)(tail[10]) << 16;
		[[fallthrough]];
	case 10:
		k2 ^= (uint64_t)(tail[9]) << 8;
		[[fallthrough]];
	case 9:
		k2 ^= (uint64_t)(tail[8]) << 0;
		k2 *= c2;
		k2 = ROTL64(k2, 33);
		k2 *= c1;
		h2 ^= k2;
		[[fallthrough]];
	case 8:
		k1 ^= (uint64_t)(tail[7]) << 56;
		[[fallthrough]];
	case 7:
		k1 ^= (uint64_t)(tail[6]) << 48;
		[[fallthrough]];
	case 6:
		k1 ^= (uint64_t)(tail[5]) << 40;
		[[fallthrough]];
	case 5:
		k1 ^= (uint64_t)(tail[4]) << 32;
		[[fallthrough]];
	case 4:
		k1 ^= (uint64_t)(tail[3]) << 24;
		[[fallthrough]];
	case 3:
		k1 ^= (uint64_t)(tail[2]) << 16;
		[[fallthrough]];
	case 2:
		k1 ^= (uint64_t)(tail[1]) << 8;
		[[fallthrough]];
	case 1:
		k1 ^= (uint64_t)(tail[0]) << 0;
		k1 *= c1;
		k1 = ROTL64(k1, 31);
		k1 *= c2;
		h1 ^= k1;
	};

	//----------
	// finalization

	h1 ^= len;
	h2 ^= len;

	h1 += h2;
	h2 += h1;

	h1 = moremur(h1);
	h2 = moremur(h2);

	h1 += h2;
	h2 += h1;

	return h1 ^ ROTL64(h2, 7);
}
