// SHA-256 and HMAC-SHA256 for signing RetroAchievements unlock records and
// sets (FIPS 180-4, RFC 2104).  Header-only: the loader (ARM9) and the
// engine (ARM7) each build their own copy.
#ifndef RA_SHA256_H
#define RA_SHA256_H

#ifndef RA_SHA256_NO_NDS_TYPES
#include <nds/ndstypes.h>
#endif

// RA_SHA256_LOW_STACK: working buffers static (not reentrant), for callers
// on a small stack such as the ARM7 card engine's
#ifdef RA_SHA256_LOW_STACK
#define RA_SHA_LOCAL static
#else
#define RA_SHA_LOCAL
#endif

typedef struct {
	u32 h[8];
	u8 block[64];
	u32 used;       // bytes in block
	u32 bitsLow, bitsHigh;
} RaSha256;

static const u32 raSha256K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define RA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void raSha256Block(RaSha256* s, const u8* p) {
	RA_SHA_LOCAL u32 w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		const u32 s0 = RA_ROR(w[i - 15], 7) ^ RA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const u32 s1 = RA_ROR(w[i - 2], 17) ^ RA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
	u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 64; i++) {
		const u32 t1 = h + (RA_ROR(e, 6) ^ RA_ROR(e, 11) ^ RA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + raSha256K[i] + w[i];
		const u32 t2 = (RA_ROR(a, 2) ^ RA_ROR(a, 13) ^ RA_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void raSha256Init(RaSha256* s) {
	static const u32 iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	for (int i = 0; i < 8; i++) s->h[i] = iv[i];
	s->used = 0;
	s->bitsLow = s->bitsHigh = 0;
}

static void raSha256Update(RaSha256* s, const void* data, u32 size) {
	const u8* p = (const u8*)data;
	const u32 bits = size << 3;
	s->bitsHigh += (size >> 29) + ((s->bitsLow + bits) < s->bitsLow);
	s->bitsLow += bits;
	while (size > 0) {
		u32 n = 64 - s->used;
		if (n > size) n = size;
		for (u32 i = 0; i < n; i++) s->block[s->used + i] = p[i];
		s->used += n;
		p += n;
		size -= n;
		if (s->used == 64) {
			raSha256Block(s, s->block);
			s->used = 0;
		}
	}
}

static void raSha256Final(RaSha256* s, u8 out[32]) {
	const u32 low = s->bitsLow, high = s->bitsHigh;
	const u8 pad = 0x80, zero = 0;
	raSha256Update(s, &pad, 1);
	while (s->used != 56) raSha256Update(s, &zero, 1);
	u8 length[8];
	for (int i = 0; i < 4; i++) {
		length[i] = (u8)(high >> (24 - i * 8));
		length[4 + i] = (u8)(low >> (24 - i * 8));
	}
	raSha256Update(s, length, 8);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (u8)(s->h[i] >> 24);
		out[i * 4 + 1] = (u8)(s->h[i] >> 16);
		out[i * 4 + 2] = (u8)(s->h[i] >> 8);
		out[i * 4 + 3] = (u8)s->h[i];
	}
}

// HMAC-SHA256 with a 32-byte key
static void raHmacSha256(const u8 key[32], const void* data, u32 size, u8 out[32]) {
	RA_SHA_LOCAL u8 pad[64], inner[32];
	RA_SHA_LOCAL RaSha256 s;
	for (int i = 0; i < 64; i++) pad[i] = (i < 32 ? key[i] : 0) ^ 0x36;
	raSha256Init(&s);
	raSha256Update(&s, pad, 64);
	raSha256Update(&s, data, size);
	raSha256Final(&s, inner);
	for (int i = 0; i < 64; i++) pad[i] = (i < 32 ? key[i] : 0) ^ 0x5c;
	raSha256Init(&s);
	raSha256Update(&s, pad, 64);
	raSha256Update(&s, inner, 32);
	raSha256Final(&s, out);
}

#undef RA_ROR
#undef RA_SHA_LOCAL

#endif // RA_SHA256_H
