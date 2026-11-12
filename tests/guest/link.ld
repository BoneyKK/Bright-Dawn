ENTRY(test_table)
SECTIONS {
  . = 0x02000000;
  .table : { KEEP(*(.table)) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
