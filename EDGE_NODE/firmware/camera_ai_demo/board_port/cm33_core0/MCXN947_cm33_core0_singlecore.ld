/*
** ###################################################################
**     Single-core linker script for MCXN947_cm33_core0
**     Ultralight Face Detection (UltraFace INT8) + SmartDMA Camera + LCD Preview
**
**     - m_data sized to 0x00067000 (412 KB in SRAM0..8)
**       Fits both UltraFace Tensor Arena (244 KB) and Camera Frame Buffer (150 KB)
**     - m_flash1 holds .model (UltraFace weights, 293 KB)
**     - m_sramx (96 KB) is 100% UNUSED by CPU, completely dedicated to SmartDMA
** ###################################################################
*/

/* Entry Point */
ENTRY(Reset_Handler)

HEAP_SIZE  = DEFINED(__heap_size__)  ? __heap_size__  : 0x0400;
STACK_SIZE = DEFINED(__stack_size__) ? __stack_size__ : 0x1800;
RPMSG_SHMEM_SIZE = 0;

TEXT_START = DEFINED(__qspi_xip__)  ? 0x80001000 : 0x00000000;
TEXT_SIZE  = DEFINED(__qspi_xip__)  ? 0x0FFFF000 : 0x000C0000;

/* Specify the memory areas */
MEMORY
{
  m_interrupts          (RX)  : ORIGIN = TEXT_START, LENGTH = 0x00000400
  m_text                (RX)  : ORIGIN = TEXT_START + 0x00000400, LENGTH = TEXT_SIZE - 0x00000400
  m_core1_image         (RX)  : ORIGIN = 0x000C0000, LENGTH = 0x00040000
  m_stack               (RW)  : ORIGIN = 0x20000000, LENGTH = 0x00001000
  m_data                (RW)  : ORIGIN = 0x20001000, LENGTH = 0x00067000
  m_flash1              (RX)  : ORIGIN = 0x00100000, LENGTH = 0x00100000
  m_sramx               (RW)  : ORIGIN = 0x04000000, LENGTH = 0x00018000
  m_flash_config        (RX)  : ORIGIN = 0x80000400, LENGTH = 0x00000400
  m_usb_sram            (RW)  : ORIGIN = 0x400BA000, LENGTH = 0x00001000
}

/* Define output sections */
SECTIONS
{
  .flash_config :
  {
    . = ALIGN(4);
    __FLASH_BASE = .;
    KEEP(* (.flexspi_fcb))     /* FCB section */
    . = ALIGN(4);
  } > m_flash_config

  /* The startup code goes first into internal flash */
  .interrupts :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))     /* Startup code */
    . = ALIGN(4);
  } > m_interrupts

  /* The program code and other data goes into internal flash */
  .text :
  {
    . = ALIGN(4);
    *(.text)                 /* .text sections (code) */
    *(.text*)                /* .text* sections (code) */
    *(.rodata)               /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)              /* .rodata* sections (constants, strings, etc.) */
    *(.glue_7)               /* glue arm to thumb code */
    *(.glue_7t)              /* glue thumb to arm code */
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
  } > m_text

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > m_text

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > m_text

 .ctors :
  {
    __CTOR_LIST__ = .;
    KEEP (*crtbegin.o(.ctors))
    KEEP (*crtbegin?.o(.ctors))
    KEEP (*(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    __CTOR_END__ = .;
  } > m_text

  .dtors :
  {
    __DTOR_LIST__ = .;
    KEEP (*crtbegin.o(.dtors))
    KEEP (*crtbegin?.o(.dtors))
    KEEP (*(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    __DTOR_END__ = .;
  } > m_text

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } > m_text

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } > m_text

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } > m_text

  __etext = .;    /* define a global symbol at end of code */
  __DATA_ROM = .; /* Symbol is used by startup for data initialization */

  .data : AT(__DATA_ROM)
  {
    . = ALIGN(4);
    __DATA_RAM = .;
    __data_start__ = .;      /* create a global symbol at data start */
    *(.ramfunc*)             /* for functions in ram */
    *(.data)                 /* .data sections */
    *(.data*)                /* .data* sections */
    *(NonCacheable.init)     /* NonCacheable init section */
    *(NonCacheable)          /* NonCacheable section */
    *(CodeQuickAccess)       /* quick access code section */
    *(DataQuickAccess)       /* quick access data section */
    KEEP(*(.jcr*))
    . = ALIGN(4);
    __data_end__ = .;        /* define a global symbol at data end */
  } > m_data

  __DATA_END = __DATA_ROM + (__data_end__ - __data_start__);
  text_end = ORIGIN(m_text) + LENGTH(m_text);
  ASSERT(__DATA_END <= text_end, "region m_text overflowed with text and data")

  /* Uninitialized data section */
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    . = ALIGN(4);
    __START_BSS = .;
    __bss_start__ = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
    __END_BSS = .;
  } > m_data

  .heap :
  {
    . = ALIGN(8);
    __end__ = .;
    PROVIDE(end = .);
    __HeapBase = .;
    . += HEAP_SIZE;
    __HeapLimit = .;
    __heap_limit = .; /* Add for _sbrk */
  } > m_data

  .stack :
  {
    . = ALIGN(8);
    __StackLimit = .;
    . += STACK_SIZE;
    __StackTop = .;
    PROVIDE(__stack = __StackTop);
  } > m_stack

  m_usb_bdt (NOLOAD) :
  {
    . = ALIGN(512);
    *(m_usb_bdt)
  } > m_usb_sram

  m_usb_global (NOLOAD) :
  {
    *(m_usb_global)
  } > m_usb_sram

  .ARM.attributes 0 : { *(.ARM.attributes) }

  ASSERT(__HeapLimit <= (ORIGIN(m_data) + LENGTH(m_data)), "region m_data overflowed with heap")

  /* 0x04000000..0x04001000 (4 KB) is reserved for SmartDMA camera firmware
   * loaded at runtime via SMARTDMA_InstallFirmware(). The remaining 92 KB of
   * m_sramx (0x04001000..0x04018000) is free for NOLOAD scratch/DMA buffers. */
  .sramx_bss (NOLOAD) :
  {
    . = ORIGIN(m_sramx) + 0x1000;
    . = ALIGN(16);
    __sramx_bss_start__ = .;
    *(.sramx_bss)
    *(.sramx_bss*)
    . = ALIGN(16);
    __sramx_bss_end__ = .;
  } > m_sramx

  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } > m_sramx

  .model :
  {
    *(.model)
    *(.model*)
  } > m_flash1
}
