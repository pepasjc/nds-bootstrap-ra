@ RetroAchievements ARM11 program for TWPatch's TwlBg (RTCom): TwlBg calls
@ the first instruction with the byte the DS side sent (r0) and passes back
@ the byte returned.  See main.c.
	.section ".entry", "ax"
	.arm
	.global _start
_start:
	push	{r1-r12, lr}
	bl	raUcodeCommand
	pop	{r1-r12, pc}
