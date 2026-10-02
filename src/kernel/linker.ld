/* silicon kernel - flat image loaded at 1 MiB by stage 2 */
ENTRY(kentry)

SECTIONS
{
    . = 0x100000;

    .text : {
        *(.entry)
        *(.text)
        *(SORT(.text$*))
        *(.text.*)
    }

    .rdata : {
        *(.rdata)
        *(SORT(.rdata$*))
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        __ctors_start = .;
        *(.ctors)
        *(SORT(.ctors.*))
        __ctors_end = .;
        *(.data)
        *(SORT(.data$*))
        *(.assets)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(SORT(.bss$*))
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.pdata) *(.xdata) *(.edata) *(.idata*) *(.reloc)
        *(.debug*) *(.comment) *(.drectve) *(.note*) *(.eh_frame)
    }
}
