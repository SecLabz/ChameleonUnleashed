SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)
ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx) : ORIGIN = 0x27000, LENGTH = 0x13ff0
  RAM (rwx) : ORIGIN = 0x20006000, LENGTH = 0x3a000
}
SECTIONS {
  .boot : { KEEP(*(.isr_vector)) *(.boot.text*) *(.boot.rodata*) . = ALIGN(4); } > FLASH
  .text : {
    __ram_text_start = .;
    *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
  } > RAM AT> FLASH
  __ram_text_load = LOADADDR(.text);
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > RAM AT> FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; __ram_text_end = .; } > RAM AT> FLASH
  .data : {
    __data_start__ = .; *(.data*) . = ALIGN(4); __data_end__ = .;
  } > RAM AT> FLASH
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : {
    . = ALIGN(512); __bss_start__ = .; *(.bss.recovery_vectors) *(.bss*) *(COMMON)
    . = ALIGN(4); __bss_end__ = .;
  } > RAM
  __StackTop = ORIGIN(RAM) + LENGTH(RAM);
  __StackLimit = __StackTop - 8192;
  __HeapBase = __bss_end__; __HeapLimit = __HeapBase;
  PROVIDE(end = __HeapBase); PROVIDE(__end__ = __HeapBase); PROVIDE(__stack = __StackTop);
  ASSERT(__StackLimit >= __bss_end__, "Recovery RAM and stack overlap")
  ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "Recovery exceeds its permanent partition")
  /DISCARD/ : { *(.stack) *(.heap) *(.eh_frame*) }
}
