/*
** ###################################################################
**     Dual-core fork of the vendor MCXN947_cm33_core0_flash.ld
**     (mcuxsdk/devices/MCX/MCXN/MCXN947/gcc/MCXN947_cm33_core0_flash.ld).
**
**     This is a FULL REPLACEMENT of the vendor script, not an additive
**     fragment (see board_port/ei_sramx.ld for why an additive `-T`
**     fragment can't do this: repartitioning m_data's boundary requires
**     redefining the MEMORY block itself, which additive fragments can't
**     safely do - see WORKLOG.md's "m_data_ext.ld" incident, which is
**     exactly why this project never widens m_data via a second -T
**     script). Only the MEMORY block changes from the vendor original:
**       - m_data shrinks from (0x20000000, 0x4E000) to (0x20000000, 0x28000)
**         to make room for...
**       - ...a new m_shared region (0x20028000, 0x26000) - see
**         source/shared/ipc_layout.h, which MUST be kept in sync with the
**         two numbers above by hand (no automated check).
**       - Stage 5 (WORKLOG.md): m_data widened by 0x4000 (16KB) at
**         m_shared's expense, reclaimed from ipc_layout.h's now-removed AI
**         crop buffer (confirmed dead: AI_MODEL_RunInference() does its
**         own internal resize/crop, nothing ever read that buffer) - core0
**         needs the room for the NPU's 120KB static tensor arena, which
**         did not fit the original 0x24000 (144KB) m_data at all.
**     m_sramx, m_core1_image, m_text, m_interrupts, m_flash1,
**     m_flash_config and m_usb_sram are all UNCHANGED from the vendor
**     script. RPMSG_SHMEM_SIZE/rpmsg_sh_mem are kept only because removing
**     them would touch more of the file than necessary for no benefit -
**     this project doesn't define __use_shmem__, so RPMSG_SHMEM_SIZE
**     always evaluates to 0 and rpmsg_sh_mem is a zero-length no-op region.
** ###################################################################
*/

/* Entry Point */
ENTRY(Reset_Handler)

HEAP_SIZE  = DEFINED(__heap_size__)  ? __heap_size__  : 0x0400;
STACK_SIZE = DEFINED(__stack_size__) ? __stack_size__ : 0x0800;
RPMSG_SHMEM_SIZE = DEFINED(__use_shmem__) ? 0x2000 : 0;

TEXT_START = DEFINED(__qspi_xip__)  ? 0x80001000 : 0x00000000;
TEXT_SIZE = DEFINED(__qspi_xip__)  ? 0x0FFFF000 : 0x000C0000;

/* Specify the memory areas */
MEMORY
{
  m_interrupts          (RX)  : ORIGIN = TEXT_START, LENGTH = 0x00000400
  m_text                (RX)  : ORIGIN = TEXT_START + 0x00000400, LENGTH = TEXT_SIZE - 0x00000400
  m_core1_image         (RX)  : ORIGIN = 0x000C0000, LENGTH = 0x00040000
  /* Shrunk from the vendor's 0x0004E000 to make room for m_shared below -
   * see source/shared/ipc_layout.h (IPC_SHARED_BASE/IPC_SHARED_SIZE MUST
   * match m_shared's ORIGIN/LENGTH here exactly). Widened again in Stage 5
   * (see the file header comment) at m_shared's expense, for the NPU
   * tensor arena. */
  m_data                (RW)  : ORIGIN = 0x20000000, LENGTH = 0x00028000 - RPMSG_SHMEM_SIZE
  /* NEW: dual-core shared RAM (camera frame buffer + result struct - see
   * source/shared/ipc_layout.h). Ends exactly at 0x2004E000, the start of
   * core1's own reserved region - core1 accesses this range via
   * fixed-address pointers, it does not need its own linker-level MEMORY
   * entry for it (see ipc_layout.h's header comment). */
  m_shared              (RW)  : ORIGIN = 0x20028000, LENGTH = 0x00026000
  rpmsg_sh_mem          (RW)  : ORIGIN = 0x2004E000 - RPMSG_SHMEM_SIZE, LENGTH = RPMSG_SHMEM_SIZE
  m_flash1              (RX)  : ORIGIN = 0x00100000, LENGTH = 0x00100000
  m_sramx               (RW)  : ORIGIN = 0x04000000, LENGTH = 0x00018000
  m_flash_config        (RX)  : ORIGIN = 0x80000400, LENGTH = 0x00000400
  m_usb_sram            (RW)  : ORIGIN = 0x400BA000, LENGTH = 0x00001000
}

/* Define output sections */
SECTIONS
{
  /* section for storing the secondary core image */
  .core1_code :
  {
     . = ALIGN(4) ;
    KEEP (*(.core1_code))
     *(.core1_code*)
     . = ALIGN(4) ;
  } > m_core1_image

  /* NOLOAD: nothing here is initialized from flash - core1's SmartDMA
   * writes the frame buffer directly, core0's AI task writes the crop
   * buffer/result struct at runtime. Placed via
   * __attribute__((section(".shared_mem"))) in whichever C file declares
   * the backing storage (see source/shared/ipc_events.c). */
  .shared_mem (NOLOAD) :
  {
     . = ALIGN(8);
     __SHARED_MEM_START__ = .;
     KEEP(*(.shared_mem))
     . = ALIGN(8);
     __SHARED_MEM_END__ = .;
  } > m_shared

  /* NOINIT section for rpmsg_sh_mem */
  .noinit_rpmsg_sh_mem (NOLOAD) : ALIGN(4)
  {
     __RPMSG_SH_MEM_START__ = .;
     *(.noinit.$rpmsg_sh_mem*)
     . = ALIGN(4) ;
     __RPMSG_SH_MEM_END__ = .;
  } > rpmsg_sh_mem

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
    . += STACK_SIZE;
  } > m_data

  m_usb_bdt (NOLOAD) :
  {
    . = ALIGN(512);
    *(m_usb_bdt)
  } > m_usb_sram

  m_usb_global (NOLOAD) :
  {
    *(m_usb_global)
  } > m_usb_sram

  /* Initializes stack on the end of block */
  __StackTop   = ORIGIN(m_data) + LENGTH(m_data);
  __StackLimit = __StackTop - STACK_SIZE;
  PROVIDE(__stack = __StackTop);

  .ARM.attributes 0 : { *(.ARM.attributes) }

  ASSERT(__StackLimit >= __HeapLimit, "region m_data overflowed with stack and heap")
}
