/*
GUEST.LD

Layout of the UWP x86-64 ILP32 (x32) guest image (tools/uwp_build.py):
linked to run at HALO_GUEST_IMAGE_BASE (0x88000000), right above the Xbox
contiguous memory window (0x80000000..0x88000000), with the image header
first. The 64-bit UWP host copies the loadable segments to these addresses
and marks the code segment executable and read-only.
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0x88000000;
	.guest_header : { KEEP(*(.guest_header "__TEXT,__guest_header")) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.guest_init : {
		__guest_init_start = .;
		KEEP(*(.guest_init .init_array .init_array.* .ctors .ctors.*))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x1000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.guest_fini .fini_array .fini_array.*) *(.eh_frame) *(.eh_frame_hdr) }
}
