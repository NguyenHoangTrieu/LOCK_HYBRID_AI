/*
 * ei_sramx.ld - additive linker fragment, NOT a replacement for the SDK's
 * board linker script.
 *
 * m_sramx (0x04000000, 96KB) is declared in the SDK's own
 * MCXN947_cm33_core0_flash.ld MEMORY block but no SECTIONS entry ever
 * places anything there, so it sits completely unused. m_data (main SRAM)
 * has no room to spare for the Edge Impulse tensor arena's primary pool -
 * so that pool is placed here instead, via the .ei_sramx section below
 * (see source/ai/ei_sramx_alloc.c for the pool + ei_malloc/ei_calloc/
 * ei_free overrides that back onto it). The current model's arena
 * (~185KB) is bigger than this 96KB bank alone, so the CPU-path build
 * also needs a large m_data overflow pool on top - see main.c's
 * s_aiScratchPool and board_port/m_data_ext.ld (which widens m_data to
 * make room for that overflow pool, on both this and the NPU path).
 *
 * Loaded via a second -T on the link line (mcux_add_armgcc_linker_script()
 * in CMakeLists.txt, called after the SDK's own linker script include) -
 * GNU ld concatenates SECTIONS commands from multiple -T scripts in the
 * order given by default, so this just needs to add one new output
 * section referencing the already-declared m_sramx region; no INSERT
 * needed (tried "INSERT AFTER .bss" first - ld rejected it with ".bss not
 * found for insert", and position relative to m_data's sections doesn't
 * matter anyway since m_sramx is a wholly separate memory bank).
 */
SECTIONS
{
    .ei_sramx (NOLOAD) :
    {
        . = ALIGN(8);
        KEEP(*(.ei_sramx))
        . = ALIGN(8);
    } > m_sramx
}
