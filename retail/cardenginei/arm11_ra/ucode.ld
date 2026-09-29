/* RetroAchievements ARM11 program: TwlBg (TWPatch, RTCom) runs uploaded
   code at 0x00201000 from its first byte.  Zeroed data is kept in the image:
   only the uploaded bytes are ours. */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
	ucode : ORIGIN = 0x00201000, LENGTH = 16K
}

SECTIONS
{
	.text : {
		KEEP(*(.entry))
		*(.text .text.* .glue_7 .glue_7t .vfp11_veneer .v4_bx)
		. = ALIGN(4);
	} >ucode
	.rodata : {
		*(.rodata .rodata.*)
		. = ALIGN(4);
	} >ucode
	.data : {
		*(.data .data.*)
		*(.bss .bss.* COMMON)
		. = ALIGN(4);
	} >ucode
	/DISCARD/ : {
		*(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*)
	}
}
