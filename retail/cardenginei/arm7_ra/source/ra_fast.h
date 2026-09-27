#ifndef RA_FAST_H
#define RA_FAST_H

#include <stdint.h>
#include "rc_runtime.h"

// A faster rc_runtime_do_frame for achievements only (see ra_fast.c).
//
// raFastPrepare() is called once every achievement has been activated: no
// achievement may be activated or deactivated afterwards (a triggered one
// just stops being evaluated).  It returns 0 if it ran out of memory, in
// which case keep calling rc_runtime_do_frame().
//
// raFastFrame() then replaces rc_runtime_do_frame().  It raises only
// RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED, once per achievement; the handler
// must not deactivate it.  ram/ramSize give the memory peek() reads, so
// plain 8/16/32-bit reads skip the callback (anything at or past ramSize
// reads 0, as peek() must).

int raFastPrepare(rc_runtime_t* runtime);
void raFastFrame(rc_runtime_t* runtime, rc_runtime_event_handler_t handler,
                 rc_runtime_peek_t peek, void* ud, const uint8_t* ram, uint32_t ramSize);

// Diagnostics: prepared achievements, how many can be skipped by a gate,
// distinct gates, memrefs computed on demand vs every frame.
struct RaFastInfo {
	uint32_t triggers, skippable, gates, lazy, always, bytes;
};
void raFastGetInfo(struct RaFastInfo* info);

extern void (*raFastPoll)(void);

#endif
