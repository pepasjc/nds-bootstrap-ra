#ifndef RA_POPUP_H
#define RA_POPUP_H

#include <nds/ndstypes.h>

// RetroAchievements unlock popup, shown through the in-game menu machinery:
// ARM7 starts the menu with sharedAddr[4] = RA_POPUP_MAGIC instead of 'MENU',
// and places the text in the in-game menu's 40K swap area, above the 39K
// menu image, so the game RAM there is backed up and restored with it.

#define RA_POPUP_MAGIC 0x55504152 // 'RAPU'
#define RA_MENU_MAGIC 0x4E4D4152 // 'RAMN': achievements list instead
#define RA_POPUP_OFFSET 0x9C00 // from INGAME_MENU_LOCATION
#define RA_POPUP_TITLE_LEN 120

struct RaPopup {
	u32 magic;
	u32 points;
	char title[RA_POPUP_TITLE_LEN];
};

#endif // RA_POPUP_H
