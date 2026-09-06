/*
** ###################################################################
**     Dual-core fork of the vendor MCXN947_cm33_core1_ram.ld
**     (mcuxsdk/devices/MCX/MCXN/MCXN947/gcc/MCXN947_cm33_core1_ram.ld) -
**     the "_ram" variant is the one actually used by the
**     CORE1_IMAGE_COPY_TO_RAM boot path (confirmed by building NXP's own
**     rpmsg_lite_pingpong_rtos secondary-core example for this board and
**     reading its real link command: `-T .../MCXN947_cm33_core1_ram.ld`,
**     NOT `_flash.ld` - core1's code is copied into and executed from RAM
**     at 0x2004E000, it is never XIP'd from flash on this path).
**
**     core1's TOTAL region (0x2004E000, length 0x1A000 = 104KB) is
**     UNCHANGED - that boundary is fixed by hardware (the power domain
**     covering 0x2004E000-0x20068000 is only enabled once core1 boots,
**     see ARCHITECTURE.md/WORKLOG.md), not something this fork can widen.
**     What IS changed from the vendor split (1KB interrupts / 51KB text /
**     52KB data) is the text/data BALANCE within that same fixed total:
**     rebalanced to 1KB / 60KB text / 43KB data. Real measurement
**     (WORKLOG.md Phase 0: building NXP's own FreeRTOS+RPMsg-Lite
**     reference example for this board measured 36,532B .text - and this
**     project doesn't even use RPMsg-Lite, only bare MCMGR events, so the
**     real number is smaller - plus ~26.6KB more for this project's own
**     camera/LCD/SD driver code, measured directly from the current
**     single-core build's own object files) showed the vendor's 51KB text
**     budget was too tight once our own driver code is added, while the
**     vendor's 52KB data budget was barely a third used (13.9KB) even
**     with RPMsg-Lite's larger footprint - so text gets some of data's
**     unused margin instead of leaving text over budget.
**
**     core1 does NOT get its own MEMORY entry for the dual-core shared
**     region (camera frame buffer + AI crop buffer + result struct) -
**     unlike core0's fork (MCXN947_cm33_core0_dualcore.ld), which declares
**     a real `m_shared` region so its linker can catch an overflow. core1
**     instead reaches that same physical RAM through the fixed-address
**     pointers in source/shared/ipc_layout.h - see that file's header
**     comment for why this asymmetry is safe (both cores can address any
**     powered RAM bank; only core0's *own* section placement needs the
**     linker's bookkeeping, not core1's raw pointer access).
**
**     Kept as a separate file (not just re-including the vendor script
**     directly) so this project's own linker scripts are self-contained
**     and carry this project's own comments, matching cm33_core0's fork.
** ###################################################################
*/

/* Entry Point */
ENTRY(Reset_Handler)

HEAP_SIZE  = DEFINED(__heap_size__)  ? __heap_size__  : 0x0400;
STACK_SIZE = DEFINED(__stack_size__) ? __stack_size__ : 0x0800;
RPMSG_SHMEM_SIZE = DEFINED(__use_shmem__) ? 0x2000 : 0;

/* Specify the memory areas */
MEMORY
{
  m_interrupts          (RX)  : ORIGIN = 0x2004E000, LENGTH = 0x00000400
  /* Rebalanced a second time (WORKLOG.md, Stage 3): 60KB/43KB (Stage 2's
     split) still overflowed text by ~1.5KB once the real camera/LCD/SPI
     driver code was added, even after trimming FreeRTOS down to just the
     modules this project actually uses (no software timers/event groups/
     stream buffers/co-routines). 64KB/39KB fits both with real margin. */
  m_text                (RX)  : ORIGIN = 0x2004E400, LENGTH = 0x00010000
  m_data                (RW)  : ORIGIN = 0x2005E400, LENGTH = 0x00009C00
  rpmsg_sh_mem          (RW)  : ORIGIN = 0x2004E000 - RPMSG_SHMEM_SIZE, LENGTH = RPMSG_SHMEM_SIZE
  m_usb_sram            (RW)  : ORIGIN = 0x400BA000, LENGTH = 0x00001000
}

/* Define output sections */
SECTIONS
{
  /* NOINIT section for rpmsg_sh_mem */
  .noinit_rpmsg_sh_mem (NOLOAD) : ALIGN(4)
  {
     __RPMSG_SH_MEM_START__ = .;
     *(.noinit.$rpmsg_sh_mem*)
     . = ALIGN(4) ;
     __RPMSG_SH_MEM_END__ = .;
  } > rpmsg_sh_mem

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
