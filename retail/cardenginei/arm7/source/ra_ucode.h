// The RetroAchievements ARM11 program (cardenginei/arm11_ra) and the card
// engine's side of it: commands over RTCom, one byte each way
#ifndef RA_UCODE_H
#define RA_UCODE_H

#define RA_UC_HELLO   0x01
#define RA_UC_LED     0x10
#define RA_UC_LED_OFF 0x11

// What RA_UC_HELLO answers: bump when the program changes, so the card
// engine uploads the new one over one left from an older build
#define RA_UC_MAGIC   0xA1

#endif
