/* The bootloader will look at this image and start execution at the symbol
   designated as the entry point. */
ENTRY(_start)

/* Tell where the various sections of the object files will be put in the final
   kernel image. */

SECTIONS
{
	KERNEL_VADDR = 0xFFFFFFFF80000000;
	. = 1M;

	_bootstrap_region_start = .;
	.multiboot ALIGN(4K) : AT(ADDR(.multiboot))
	{
    	*(.bootstrap_code)
	}

	.bootstrap_stack : ALIGN(4K)
	{
    	*(.bootstrap_stack)
	}

	_bootstrap_region_end = .;

	. = 0x0f000;
	_trampoline_region_start = .;
	.trampoline_code ALIGN(4K) : AT(ADDR(.trampoline_code))
	{
		*(.trampoline_code)
		*(.multiboot)
	  	*(.trampoline_data)
	}

	.trampoline_stack ALIGN(4K) : AT(ADDR(.trampoline_stack))
	{
    	*(.trampoline_stack)
	}

	_trampoline_region_end = .;
	
	. = KERNEL_VADDR + 2M;
  	_region_kernel_start_ = . - KERNEL_VADDR;
	/* -mcmodel=large makes clang emit large-model sections (.ltext/.lrodata/
	   .ldata/.lbss, marked SHF_X86_64_LARGE). They must be matched explicitly
	   or lld places them as orphans at a low address, leaving most of the
	   kernel below the -2GB window and breaking calls/relocations. */
	.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VADDR)
	{
		*(.entry_point)
		*(.tramp_handler)
		*(.tramp_handler_code)
		*(.text .text.* .ltext .ltext.*)
	}

	/* Read-only data. */
	.rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VADDR)
	{
		*(.rodata .rodata.* .lrodata .lrodata.*)
	}

	/* Read-write data (initialized) */
	.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VADDR)
	{
		*(.data .data.* .ldata .ldata.*)
	}

	/* Read-write data (uninitialized) and stack */
	.bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VADDR)
	{
		*(COMMON)
		*(.bss .bss.* .lbss .lbss.*)
	}

	/* The compiler may produce other sections, by default it will put them in
	   a segment with the same name. Simply add stuff here as needed. */
	_region_kernel_end_ = . - KERNEL_VADDR;
}
