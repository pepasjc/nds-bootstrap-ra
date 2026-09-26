@ Entry stubs for the RetroAchievements engine: run the C code on the
@ region's own stack, since the game's ARM7 IRQ stack is tiny.

	.arm
	.section .text
	.align 4

	.global raInitStub
	.type raInitStub STT_FUNC
raInitStub:
	stmfd sp!, {r4, lr}
	mov r4, sp
	ldr sp, =__ra_stack_top
	ldr r12, =raInit
	mov lr, pc
	bx r12
	mov sp, r4
	ldmfd sp!, {r4, lr}
	bx lr

	.global raFrameStub
	.type raFrameStub STT_FUNC
raFrameStub:
	stmfd sp!, {r4, lr}
	mov r4, sp
	ldr sp, =__ra_stack_top
	ldr r12, =raFrame
	mov lr, pc
	bx r12
	mov sp, r4
	ldmfd sp!, {r4, lr}
	bx lr

	.pool
