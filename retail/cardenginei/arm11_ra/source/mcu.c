// TwlBg's own MCU (I2C) functions, found by their code: TwlBg's code sits
// at 0x100000 (0x28000 bytes).  The byte patterns (and which halfwords of
// the BL instructions in them to ignore) are what TwlBg's code looks like;
// Gericom's Rtc3DS showed how to use them.  The functions are Thumb.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define CODE_START ((const u8*)0x100000)
#define CODE_SIZE  0x28000

typedef void (*LockFn)(void* unused, int unused2);
typedef int (*RegFn)(void* mcu, int reg, const u8* data, int count);

static LockFn s_lock, s_unlock;
static RegFn s_write;
static void* s_mcu;

// First place where pattern matches, ignoring the bits set in mask
static const u8* find(const u8* pattern, const u8* mask, int size, const u8* from, const u8* to)
{
	for (const u8* p = from; p + size <= to; p++) {
		int i = 0;
		while (i < size && ((p[i] ^ pattern[i]) & ~mask[i]) == 0) i++;
		if (i == size) return p;
	}
	return NULL;
}

int mcuFind(void)
{
	static const u8 lockPat[16] = { 0x0C, 0x20, 0x10, 0xB5, 0x41, 0x43, 0x02, 0x48, 0x08, 0x18, 0x00, 0xF0, 0x89, 0xF9, 0x10, 0xBD };
	static const u8 lockPat2[16] = { 0x0C, 0x20, 0x10, 0xB5, 0x41, 0x43, 0x02, 0x48, 0x08, 0x18, 0x00, 0xF0, 0x85, 0xF9, 0x10, 0xBD };
	static const u8 lockMask[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x07, 0xFF, 0x07, 0, 0 };
	static const u8 writePat[18] = { 0x08, 0xB5, 0x00, 0x93, 0x13, 0x46, 0x0A, 0x46, 0x00, 0x68, 0x03, 0x21, 0xF5, 0xF7, 0xCE, 0xFB, 0x08, 0xBD };
	static const u8 writeMask[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x03, 0xFF, 0x07, 0, 0 };
	static const u8 mcuPat[14] = { 0x70, 0xB5, 0x00, 0x21, 0x05, 0x68, 0x28, 0x46, 0x10, 0xF0, 0x5E, 0xFC, 0x04, 0x48 };
	static const u8 mcuMask[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x07, 0xFF, 0x07, 0xFF, 0 };
	const u8* end = CODE_START + CODE_SIZE;

	const u8* p = find(lockPat, lockMask, 16, CODE_START, end);
	if (!p) return 1;
	s_unlock = (LockFn)((u32)p | 1);
	p = find(lockPat2, lockMask, 16, p + 4, end);
	if (!p) return 2;
	s_lock = (LockFn)((u32)p | 1);
	p = find(writePat, writeMask, 18, CODE_START, end);
	if (!p) return 4;
	s_write = (RegFn)((u32)p | 1);
	// A function that loads the MCU object from a literal pool: its
	// address, from the LDR at +12
	p = find(mcuPat, mcuMask, 10, CODE_START, end);
	if (!p) return 5;
	s_mcu = *(void**)(((u32)p + 12 + ((u32)p[12] * 4) + 4) & ~3u);
	return 0;
}

bool mcuWrite(int reg, const u8* data, int len)
{
	s_lock(NULL, 0);
	const int r = s_write(s_mcu, reg, data, len);
	s_unlock(NULL, 0);
	return r == 0;
}
