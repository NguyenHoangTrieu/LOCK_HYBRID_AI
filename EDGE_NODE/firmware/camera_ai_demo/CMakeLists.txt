# FRDM-MCXN947 — Camera_AI_Test1: OV7670 (J9) capture -> AI hook. Default
# build is camera-only (no display, no USB) - see WORKLOG.md. Two earlier
# output paths were tried and abandoned, both kept in the tree (still build,
# just not called from main.c / not the default) in case either is
# revisited:
#   - J8 FlexIO/TFT display (source/display/, LCD_BITBANG_DIAGNOSTIC below) -
#     abandoned for signal-integrity reasons on the specific panel in hand.
#   - USB Video Class (UVC) webcam over USB HS (source/usb/,
#     USB_STREAM_DIAGNOSTIC_DISABLE below) - abandoned because SmartDMA
#     camera capture and the USB HS PHY require mutually exclusive DCDC
#     voltage levels on this chip, and this board's single USB connector is
#     hard-wired to the HS controller only (no way to use the alternative,
#     Mid-voltage-compatible USB FS module instead).
#
# Out-of-tree ("freestanding") MCUXpresso SDK application, same pattern as
# ../../touch_rgb and ../../wifi_sensing_npu: source lives here, outside the
# SDK checkout, and only refers back into it through ${SdkRootDirPath}.
#
# PROJECT_BOARD_PORT_PATH points at our own board_port/ folder, which holds
# the pin muxing, clock setup and app.h that would normally live under the
# SDK's examples/_boards/<board>/ tree. board.c/clock_config.c are NOT
# duplicated here - the SDK supplies those automatically for board=frdmmcxn947.

cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

# The SDK resolves PROJECT_BOARD_PORT_PATH against ${SdkRootDirPath}, so it
# has to be a RELATIVE path — passing an absolute one silently produces
# "/path/to/sdk/home/you/..." and the build fails to find pin_mux.c.
file(RELATIVE_PATH BOARD_PORT_REL_PATH
     ${SdkRootDirPath} ${CMAKE_CURRENT_SOURCE_DIR}/board_port)

project(camera_ai_demo LANGUAGES C CXX ASM
        PROJECT_BOARD_PORT_PATH ${BOARD_PORT_REL_PATH})

# Dual-core RTOS migration (see WORKLOG.md/ARCHITECTURE.md): core0 = AI
# inference only, core1 = camera capture + LCD/SPI display + SD card saving,
# both running FreeRTOS, MCMGR events for cross-core signaling. OFF by
# default so the existing, proven single-core bare-metal build (below) stays
# completely untouched and buildable/flashable as a rollback at every stage
# of the migration - this option only gates the small, separate bring-up
# path further down; none of the existing options/sources above are
# affected by it.
option(DUALCORE_RTOS "Boot core1 and split camera/LCD/SD (core1) from AI inference (core0) - see WORKLOG.md dual-core migration" OFF)

if(DUALCORE_RTOS)
    include(${SdkRootDirPath}/CMakeLists.txt)

    # Shared across both cores' branches below - lets board_port/cm33_core0/
    # hardware_init.c (compiled either way) skip the USB Video Class code
    # that only exists for the legacy single-core build (USB streaming is
    # already abandoned per WORKLOG.md/ARCHITECTURE.md Sec.4 - the dual-core
    # build doesn't carry it forward at all).
    mcux_add_configuration(
        CC "-DDUALCORE_RTOS=1"
    )

    # Stage 2 (WORKLOG.md): FreeRTOS kernel, pulled directly (not via the
    # CONFIG_MCUX_COMPONENT_middleware.freertos-kernel Kconfig component -
    # see source/shared/FreeRTOSConfig.h's header comment for why: that
    # component's Kconfig lives in the same shared prj.conf files the
    # legacy single-core build also reads, and FreeRTOS's port.c redefines
    # SVC_Handler/PendSV_Handler/SysTick_Handler - not worth risking on the
    # working legacy build just to save writing one config header by hand).
    # Shared between both cores' branches below since both run FreeRTOS.
    # Only the core kernel modules this project actually uses - no software
    # timers, event groups, stream buffers or co-routines anywhere in this
    # codebase (task notifications, used for IPC, don't need any of these -
    # they're embedded directly in the TCB). Trimmed after a real,
    # measured core1 m_text overflow (WORKLOG.md, Stage 3) - every one of
    # these is real code-size budget on core1's tight 60KB region.
    set(FREERTOS_ROOT ${SdkRootDirPath}/rtos/freertos/freertos-kernel-upstream)
    mcux_add_source(
        BASE_PATH ${FREERTOS_ROOT}
        SOURCES tasks.c
                list.c
                queue.c
                portable/GCC/ARM_CM33_NTZ/non_secure/port.c
                portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
                portable/GCC/ARM_CM33_NTZ/non_secure/mpu_wrappers_v2_asm.c
                portable/MemMang/heap_4.c
    )
    mcux_add_include(
        BASE_PATH ${FREERTOS_ROOT}
        INCLUDES include
                 portable/GCC/ARM_CM33_NTZ/non_secure
    )
    # This project's own lightweight MCMGR event wrapper (ISR callback ->
    # xTaskNotifyFromISR) - shared by both cores' Stage 2+ tasks.
    mcux_add_source(
        BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES source/shared/ipc_events.c source/shared/ipc_events.h
                source/shared/freertos_hooks.c
    )

    # Stage 1 (WORKLOG.md): boot-only bring-up - core0 just boots core1 via
    # MCMGR and both print a UART banner, nothing else yet. Each stage adds
    # real work (FreeRTOS in Stage 2, camera/LCD/SD on core1 in Stages 3-4,
    # AI on core0 in Stage 5) on top of this skeleton; this if/else branch
    # will grow accordingly rather than being rewritten from scratch each
    # stage.
    if(${core_id} STREQUAL "cm33_core1")
        mcux_remove_armgcc_linker_script(
            LINKER ${SdkRootDirPath}/devices/MCX/MCXN/MCXN947/gcc/MCXN947_cm33_core1_ram.ld
        )
        mcux_add_armgcc_linker_script(
            BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
            LINKER board_port/cm33_core1/MCXN947_cm33_core1_dualcore.ld
        )
        # USB Video Class gets pulled in regardless of prj.conf overrides -
        # the top-level prj.conf (shared with the legacy single-core build)
        # sets CONFIG_USB_DEVICE_CONFIG_VIDEO=1, which Kconfig re-derives
        # `default y if USB_DEVICE_CONFIG_VIDEO > 0` for the video/EHCI/PHY
        # components on every reconfigure regardless of later per-core
        # prj.conf overrides (confirmed the hard way - neither disabling
        # the derived component nor the root trigger in board_port/
        # cm33_core1/prj.conf stuck). This project never streams over USB
        # in the dual-core build (already abandoned even in the legacy
        # build, see ARCHITECTURE.md Sec.4), and it was eating real m_text
        # budget on core1's tight 60KB region for entirely dead code
        # (confirmed: nothing in this project's dual-core sources
        # references any USB API) - removing the compiled sources directly
        # is the one thing that actually worked.
        mcux_project_remove_source(
            BASE_PATH ${SdkRootDirPath}
            SOURCES middleware/usb/device/usb_device_khci.c
                    middleware/usb/device/usb_device_ehci.c
                    middleware/usb/device/usb_device_dci.c
                    middleware/usb/device/usb_device_ch9.c
                    middleware/usb/device/class/usb_device_class.c
                    middleware/usb/device/class/usb_device_video.c
                    middleware/usb/phy/usb_phy.c
        )
        # SPI_RETRY_TIMES - REQUIRED, not optional (see the matching block
        # further down this file, under the legacy build's own comment for
        # the full story): without this, fsl_lpspi.c's blocking transfers
        # compile to an unconditional `while (...) {}` with zero timeout on
        # this chip's LP_FLEXCOMM-hosted LPSPI, and a missing/dead SD card
        # hangs boot completely instead of failing gracefully. The
        # DUALCORE_RTOS branch returns before reaching the legacy copy of
        # this macro further down, so it needs repeating here.
        mcux_add_macro(
            CC "-DSPI_RETRY_TIMES=100000"
        )
        # -Os for the whole core1 image (overrides the SDK-wide -O0
        # default - confirmed appended after it on the compile command
        # line, so it wins per GCC's "last -O flag wins" rule). Added
        # after a real, measured overflow (WORKLOG.md, Stage 4): FreeRTOS
        # + MCMGR + camera/LCD/SPI/SD/FatFs genuinely doesn't fit core1's
        # fixed 104KB region at -O0, even after trimming FreeRTOS to its
        # minimum and rebalancing text/data twice already - there was no
        # rebalancing split left that could fit the total. Safe here
        # (unlike the legacy build's own -O2-is-wrong lesson for
        # lcd_bitbang.c, see that comment further down this file): none of
        # core1's code is timing-sensitive software bit-banging - the
        # camera and LCD buses are both driven by real hardware (SmartDMA,
        # LPSPI1's own baud-rate generator), so optimization can only
        # affect CPU-side speed, never the electrical signal timing.
        mcux_add_configuration(
            CC "-Os"
        )
        # SD-over-SPI protocol layer + FatFs core, pulled directly - same
        # reasoning as the legacy build's own copy of these blocks further
        # down this file (Kconfig's middleware.sdmmc.sdspi/middleware.fatfs
        # components pull in things that don't compile or collide on this
        # chip/project - see those comments for the full explanation).
        mcux_add_source(
            BASE_PATH ${SdkRootDirPath}
            SOURCES middleware/sdmmc/sdspi/fsl_sdspi.c
                    middleware/sdmmc/sdspi/fsl_sdspi.h
                    middleware/sdmmc/common/fsl_sdmmc_spec.h
        )
        mcux_add_include(
            BASE_PATH ${SdkRootDirPath}
            INCLUDES middleware/sdmmc/sdspi
                     middleware/sdmmc/common
        )
        mcux_add_source(
            BASE_PATH ${SdkRootDirPath}
            SOURCES middleware/fatfs/source/ff.c
                    middleware/fatfs/source/ff.h
                    middleware/fatfs/source/diskio.h
        )
        mcux_add_include(
            BASE_PATH ${SdkRootDirPath}
            INCLUDES middleware/fatfs/source
        )
        mcux_add_source(
            BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
            SOURCES source/main_core1.c
                    source/fault_handler.c
                    source/camera/camera_capture.c
                    source/camera/camera_capture.h
                    source/display/lcd_display.h
                    source/display/lcd_spi_hw.c
                    source/display/lcd_spi_hw.h
                    source/spi1_bus.c
                    source/spi1_bus.h
                    source/storage/sd_spi_disk.c
                    source/storage/sd_spi_disk.h
                    source/storage/snapshot.c
                    source/storage/snapshot.h
                    source/storage/ffconf.h
                    source/display/bbox_overlay.c
                    source/display/bbox_overlay.h
                    source/ai/model_runner.h
        )
        mcux_add_include(
            INCLUDES source source/shared source/camera source/display source/storage source/ai
        )
        # -O2 for the same real-hardware-confirmed reason as the legacy
        # single-core build (see the matching block further down this
        # file, under LCD_ARDUINO_HEADER_BITBANG): this driver's bus timing
        # comes from LPSPI's own hardware baud-rate generator, not software
        # instruction latency, so -O2 here only speeds up CPU-side work
        # (LCD_PushPixelsOpen()'s byte-swap loop) - it cannot make the
        # electrical SPI signal "too fast" the way it would for a bit-bang
        # driver.
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/source/display/lcd_spi_hw.c
            PROPERTIES COMPILE_OPTIONS "-O2"
        )
        # Raw core1 image for core0 to .incbin - see board_port/cm33_core0/
        # app.h's comment for the whole embed mechanism.
        mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${CONFIG_TOOLCHAIN}/core1_image.bin)
    else() # cm33_core0
        mcux_remove_armgcc_linker_script(
            LINKER ${SdkRootDirPath}/devices/MCX/MCXN/MCXN947/gcc/MCXN947_cm33_core0_flash.ld
        )
        mcux_add_armgcc_linker_script(
            BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
            LINKER board_port/cm33_core0/MCXN947_cm33_core0_dualcore.ld
        )
        mcux_add_configuration(
            CC "-DCORE1_IMAGE_COPY_TO_RAM"
        )
        # fsl_incbin.S pulled directly (not via the CONFIG_MCUX_COMPONENT_
        # utility.incbin Kconfig component) - same reasoning as this
        # project's other direct-pull sources (fsl_sdspi.c, ff.c below):
        # avoids the Kconfig component ever being (accidentally) enabled
        # for the plain single-core build, which would require a
        # core1_image.bin to exist that doesn't in that configuration.
        mcux_add_source(
            BASE_PATH ${SdkRootDirPath}
            SOURCES components/misc_utilities/fsl_incbin.S
        )
        mcux_add_source(
            BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
            SOURCES source/main_core0.c
                    source/fault_handler.c
                    source/ai/model_runner.h
                    source/ai/model_runner_npu.cpp
        )
        mcux_add_include(
            INCLUDES source source/shared source/ai
        )
        # Neutron NPU inference backend, same choice the legacy single-core
        # build defaults to (AI_MODEL_USE_NPU=ON there - see that option's
        # comment further down this file for the full backend-choice
        # story). No build-time OFF switch here (unlike the legacy build) -
        # Stage 5 (WORKLOG.md) only ever tested the NPU path; the CPU+
        # CMSIS-NN/Edge-Impulse-SDK path would need its own separate wiring
        # (ei_sramx_alloc.c/ei_debug_porting.c/model_runner.cpp, the whole
        # source/ai/edge_impulse/ tree) if ever needed here.
        set(TFLM_ROOT ${SdkRootDirPath}/middleware/eiq/tensorflow-lite)
        set(NEUTRON_ROOT ${SdkRootDirPath}/middleware/eiq/neutron)
        mcux_add_source(
            BASE_PATH ${TFLM_ROOT}
            SOURCES tensorflow/lite/micro/kernels/neutron/neutron.cpp
                    tensorflow/lite/micro/kernels/neutron/neutron.h
                    tensorflow/lite/micro/micro_time.cpp
                    tensorflow/lite/micro/debug_log.cpp
        )
        # Vendored NXP code, not warning-clean under this toolchain's -Wall -
        # same exemption the legacy build's own copy of these exact files
        # needs (see that block further down this file).
        set_source_files_properties(
            ${TFLM_ROOT}/tensorflow/lite/micro/kernels/neutron/neutron.cpp
            ${TFLM_ROOT}/tensorflow/lite/micro/micro_time.cpp
            ${TFLM_ROOT}/tensorflow/lite/micro/debug_log.cpp
            PROPERTIES COMPILE_OPTIONS "-Wno-error"
        )
        mcux_add_include(
            BASE_PATH ${SdkRootDirPath}
            INCLUDES middleware/eiq/tensorflow-lite
                     middleware/eiq/tensorflow-lite/third_party/flatbuffers/include
                     middleware/eiq/tensorflow-lite/third_party/gemmlowp
                     middleware/eiq/tensorflow-lite/third_party/ruy
                     middleware/eiq/neutron/driver/include
                     middleware/eiq/neutron/common/include
        )
        # Must match the prebuilt libtflm.a's own ABI - same macros the
        # legacy build's AI_MODEL_USE_NPU branch sets.
        mcux_add_macro(
            CC "-DTF_LITE_STATIC_MEMORY -DKERNELS_OPTIMIZED_FOR_SPEED"
            CX "-DTF_LITE_STATIC_MEMORY -DKERNELS_OPTIMIZED_FOR_SPEED"
        )
        target_link_libraries(${MCUX_SDK_PROJECT_NAME} PRIVATE
            ${TFLM_ROOT}/lib/cm33/armgcc/libtflm.a
            ${NEUTRON_ROOT}/mcxn/libNeutronDriver.a
            ${NEUTRON_ROOT}/mcxn/libNeutronFirmware.a
        )
        # Assembler include path so fsl_incbin.S's `.incbin "core1_image.bin"`
        # finds core1's build output - core1 MUST be built first into a
        # sibling `core1/` directory next to this build's own directory
        # (build.sh does this - see WORKLOG.md).
        mcux_add_include(
            TOOLCHAINS armgcc
            BASE_PATH ${APPLICATION_BINARY_DIR}
            INCLUDES ../core1/${CONFIG_TOOLCHAIN}/
        )
        mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)
    endif()

    return()
endif()

include(${SdkRootDirPath}/CMakeLists.txt)

# Edge Impulse tensor arena (see source/ai/ei_sramx_alloc.c - primary pool
# is m_sramx's full 96KB, current model's ~185KB arena needs a large
# overflow on top, see main.c's s_aiScratchPool) doesn't fit in m_data
# alongside the camera/LCD framebuffers - this fragment adds an output
# section placing the primary pool in m_sramx instead (declared but
# otherwise unused by the SDK's own linker script). Additive only
# (INSERT AFTER) - does not replace/duplicate the SDK's board linker
# script, so must come after include(${SdkRootDirPath}/CMakeLists.txt)
# above, which is what actually selects the base -T script.
mcux_add_armgcc_linker_script(
    BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    LINKER board_port/ei_sramx.ld
)

# board_port/m_data_ext.ld (widening m_data into core1's reserved RAM) was
# tried here and REVERTED (2026-08-25) - confirmed BAD on real hardware,
# not just "unconfirmed": flashed fine (pyOCD reported success) but the
# board then produced zero UART output and became undebuggable (SWD
# connect failures - DebugPortStart/ResetSystem faults - that needed a
# full chip mass-erase via NXP's nxpdebugmbox Debug Mailbox tool to
# recover from), consistent with a hang during .bss zero-init the moment
# startup code touched that "reclaimed" region - it is NOT safely usable
# plain SRAM on this silicon, despite looking that way from the linker
# script/datasheet alone. Do not re-add without a much more solid basis
# than "the address range looks contiguous and nothing disables it in
# this SDK's boot code" - that reasoning was wrong once already. See
# WORKLOG.md's top entry for the full incident.

# Default stack (STACK_SIZE in the SDK's linker script) is only 2KB - fine
# for plain GPIO/UART firmware, but TFLite Micro/CMSIS-NN's call chain goes
# many frames deep, with individual frames that are themselves large (e.g.
# arm_nn_mat_mult_nt_t_s8's ~2.7KB of locals for its unrolled q31
# accumulators). First bumped to 4KB, which was NOT enough - see
# WORKLOG.md "Bug #2" / "root cause found" for the full diagnosis: the
# fault was misread as a CMSIS-NN pointer-alignment UsageFault (UFSR bit
# 8, UNALIGNED) when it was actually STKOF (UFSR bit 4, hardware stack-
# limit check - CFSR 0x00100000 is bit 20 of CFSR = bit 4 of UFSR, not bit
# 8), confirmed by disassembly: the fault PC was the function's prologue
# `subw sp, sp, #2780` itself, not a load/store. Bumped to 16KB now that
# s_aiScratchPool (main.c) was shrunk from 150KB to 16KB to make room in
# m_data (the tensor arena fits the primary 96KB m_sramx pool without
# needing a large overflow pool - see the comment there). The linker's own
# ASSERT(__StackLimit >= __HeapLimit) will fail the build rather than
# silently overflowing if this doesn't fit.
mcux_add_linker_symbol(
    SYMBOLS "__stack_size__=0x4000"
)

# The panel in hand only has an 8-bit data bus (LCD_D0..D7), not the 16-bit
# bus the J8 header's full FlexIO wiring supports. FLEXIO_MCULCD_DATA_BUS_WIDTH
# is a compile-time macro read by the SDK's own fsl_flexio_mculcd.c, so it has
# to be set here (globally) rather than in our own source.
mcux_add_macro(
    CC "-DFLEXIO_MCULCD_DATA_BUS_WIDTH=8"
)

# CONFIRMED on real hardware (2026-08-25): boot hung completely (no serial
# output past "LCD: bit-bang GPIO on the Arduino header", survived multiple
# resets) the first time SNAPSHOT_Init() -> f_mount() -> disk_initialize()
# (source/storage/sd_spi_disk.c) actually ran on a board without the SD
# card wired/ready yet. Root cause: drivers/lpflexcomm/lpspi/fsl_lpspi.c's
# LPSPI_MasterTransferBlocking() polls its TX/RX FIFO flags in a loop
# that's only bounded if the SPI_RETRY_TIMES macro is defined - see its
# `#if SPI_RETRY_TIMES ... #else while (...) {} #endif` pattern, repeated
# ~19 times in that file. SPI_RETRY_TIMES is a Kconfig option, but it only
# exists under drivers/lpspi/Kconfig's plain "driver.lpspi" component -
# this board's LPSPI1 is gated by "driver.lpflexcomm_lpspi" instead (see
# prj.conf), which has no such Kconfig knob at all, so the macro was never
# defined and every one of those loops compiled to a genuine `while (1) {}`
# with zero timeout. SNAPSHOT_Init() was designed to fail gracefully (log
# once, skip snapshots for the rest of the session) when the card isn't
# ready - that path was unreachable because the SPI transfer underneath it
# never returned at all. Fixing the retry loops with a real, bounded define
# turns any SD/wiring problem into the intended graceful failure instead of
# a hard hang - it does NOT fix whatever the underlying SD wiring/card
# issue is (see README.md's Known Limitations for that).
mcux_add_macro(
    CC "-DSPI_RETRY_TIMES=100000"
)

# Which LCD connector/transport to build for - see WORKLOG.md "LCD history"
# for the full story. Default (ON): hardware LPSPI1 on the Arduino header,
# driving the current 2.4" SPI TFT module (ILI9341-family) - replaces this
# project's original 8-bit-parallel Arduino-header panel, see
# source/display/lcd_spi_hw.c. That bus is SHARED with the module's onboard
# microSD slot and XPT2046 touch controller (see source/spi1_bus.h) - when
# this option is ON, TOUCH_BACKEND_SOURCE below additionally pulls in the
# touch driver (source/display/touch_xpt2046.c), since its pin macros only
# exist in app.h's DEMO_LCD_ARDUINO_HEADER=1 branch. Set OFF to instead
# build for J8 (abandoned - signal-integrity issues never fully resolved on
# the FlexIO hardware-bus path, no touch/SD sharing there either);
# LCD_BITBANG_DIAGNOSTIC then additionally selects J8 bit-bang
# (diagnostic-only, see source/display/lcd_bitbang.c) vs. J8 FlexIO.
option(LCD_ARDUINO_HEADER_BITBANG "Drive the TFT via hardware SPI on the Arduino header, shared with the microSD/touch (fallback from J8 - see WORKLOG.md)" ON)
if(LCD_ARDUINO_HEADER_BITBANG)
    set(LCD_BACKEND_SOURCE source/display/lcd_spi_hw.c source/display/lcd_spi_hw.h)
    set(TOUCH_BACKEND_SOURCE source/display/touch_xpt2046.c source/display/touch_xpt2046.h)
    mcux_add_macro(CC "-DDEMO_LCD_ARDUINO_HEADER=1 -DDEMO_LCD_BITBANG=1")
else()
    option(LCD_BITBANG_DIAGNOSTIC "Bit-bang the J8 LCD bus via GPIO instead of FlexIO (diagnostic)" OFF)
    set(TOUCH_BACKEND_SOURCE "")
    if(LCD_BITBANG_DIAGNOSTIC)
        set(LCD_BACKEND_SOURCE source/display/lcd_bitbang.c source/display/lcd_bitbang.h)
        mcux_add_macro(CC "-DDEMO_LCD_ARDUINO_HEADER=0 -DDEMO_LCD_BITBANG=1")
    else()
        set(LCD_BACKEND_SOURCE source/display/lcd_flexio_mculcd.c source/display/lcd_flexio_mculcd.h)
        mcux_add_macro(CC "-DDEMO_LCD_ARDUINO_HEADER=0 -DDEMO_LCD_BITBANG=0")
    endif()
endif()

# USB streaming (UVC webcam over USB HS) is ABANDONED as of this entry - see
# WORKLOG.md "USB streaming pipeline abandoned" for the full reasoning. Root
# cause: SmartDMA camera capture only works with DCDC at Mid voltage, USB HS
# PHY only works at Overdrive, and FRDM-MCXN947's single USB Type-C connector
# (J11) is hard-wired to the HS controller only - the chip's separate USB FS
# module (which wouldn't need Overdrive) has no accessible connector on this
# board (confirmed in NXP's UM12018 board user manual, section 2.3), so
# there's no way to route around the conflict without a hardware rework. A
# software time-multiplex workaround (periodic Mid/Overdrive switching) was
# built and confirmed stable on real hardware, but the underlying conflict is
# a genuine chip/board limitation, not something fixable from this project's
# code - same status as the abandoned LCD path below (source/display/), kept
# in the tree in case NXP ever publishes a fix or this needs revisiting.
#
# ON by default now (camera+AI loop only, no USB) - this is the
# actively-used build. Set to OFF to opt back into the abandoned USB
# streaming path (still there, still builds, just not the default anymore).
option(USB_STREAM_DIAGNOSTIC_DISABLE "Skip USB bring-up entirely - camera+AI loop only, no USB (USB streaming is abandoned, see WORKLOG.md)" ON)
if(USB_STREAM_DIAGNOSTIC_DISABLE)
    mcux_add_macro(CC "-DDEMO_USB_STREAM_DISABLE=1")
else()
    mcux_add_macro(CC "-DDEMO_USB_STREAM_DISABLE=0")
endif()

# Diagnostic-only build: skip AI entirely and stream the raw camera feed
# straight to the LCD, as fast as frames arrive (no ~1.27s/inference
# stall) - for physically focusing the lens while watching the panel.
# OFF by default (normal camera+AI+status-text build).
option(LCD_CAMERA_PREVIEW "Skip AI, stream the raw camera feed straight to the LCD - for focusing the lens" OFF)
if(LCD_CAMERA_PREVIEW)
    mcux_add_macro(CC "-DDEMO_LCD_CAMERA_PREVIEW=1")
else()
    mcux_add_macro(CC "-DDEMO_LCD_CAMERA_PREVIEW=0")
endif()

# Which AI inference backend to build: this chip's Neutron NPU
# (model_runner_npu.cpp, default - ~3.3ms/inference) via NXP's own
# middleware/eiq/tensorflow-lite + middleware/eiq/neutron, bypassing the
# Edge Impulse SDK entirely, or CPU+CMSIS-NN via the Edge Impulse SDK
# (model_runner.cpp, ~1.27s/inference - set OFF to use this instead) - see
# WORKLOG.md "NPU (Neutron) plan" for the full story (both paths implement
# the identical model_runner.h API, so main.c doesn't change either way,
# and both print the same DWT-based "total classifier time" line for a
# direct on/off comparison).
# Was briefly defaulted OFF (2026-08-25) when the deploy-version-1 face
# model (96x96, alpha=0.35) needed a >=166KB NPU arena that did not fit
# this chip's m_data at all - see WORKLOG.md's top entry. Back to ON
# (2026-08-25, same day) now that the model was retrained smaller
# (deploy version 2, 72x72 input, ~93.6KB NPU arena estimate) - both
# backends now fit stock m_data comfortably, see the kTensorArenaSize
# comment in model_runner_npu.cpp and the s_aiScratchPool comment in
# main.c for the exact budget math. If a future model swap makes this
# not fit again, don't just leave NPU as the (broken) default - flip
# this OFF and say so clearly, like this comment does.
option(AI_MODEL_USE_NPU "Run inference on the Neutron NPU (model_runner_npu.cpp) instead of CPU+CMSIS-NN via the Edge Impulse SDK (model_runner.cpp) - see WORKLOG.md" ON)
if(AI_MODEL_USE_NPU)
    set(AI_MODEL_RUNNER_SOURCE source/ai/model_runner_npu.cpp)
    # main.c uses this to skip allocating the CPU-path's m_data overflow
    # scratch pool (source/ai/ei_sramx_alloc.c is compiled unconditionally
    # into both builds, but only model_runner.cpp's ei_run_classifier()
    # call actually uses it) - that pool would otherwise sit there unused,
    # competing with model_runner_npu.cpp's own (much larger, for this
    # 96x96 model) static tensor arena for the same m_data budget.
    mcux_add_macro(CC "-DDEMO_AI_MODEL_USE_NPU=1" CX "-DDEMO_AI_MODEL_USE_NPU=1")
else()
    set(AI_MODEL_RUNNER_SOURCE source/ai/model_runner.cpp)
    mcux_add_macro(CC "-DDEMO_AI_MODEL_USE_NPU=0" CX "-DDEMO_AI_MODEL_USE_NPU=0")
endif()

mcux_add_source(
    BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    SOURCES source/main.c
            source/fault_handler.c
            source/spi1_bus.c
            source/spi1_bus.h
            source/camera/camera_capture.c
            source/camera/camera_capture.h
            source/display/lcd_display.h
            source/display/bbox_overlay.c
            source/display/bbox_overlay.h
            source/display/font5x7.h
            source/display/text_overlay.c
            source/display/text_overlay.h
            ${LCD_BACKEND_SOURCE}
            ${TOUCH_BACKEND_SOURCE}
            ${AI_MODEL_RUNNER_SOURCE}
            source/ai/model_runner.h
            source/ai/model_data.h
            source/ai/ei_sramx_alloc.c
            source/ai/ei_debug_porting.c
            source/usb/usb_device_descriptor.c
            source/usb/usb_device_descriptor.h
            source/usb/usb_video_camera.c
            source/usb/usb_video_camera.h
            source/storage/sd_spi_disk.c
            source/storage/sd_spi_disk.h
            source/storage/snapshot.c
            source/storage/snapshot.h
            source/storage/ffconf.h
)

# SD-over-SPI protocol layer (fsl_sdspi.c) pulled directly, NOT via
# CONFIG_MCUX_COMPONENT_middleware.sdmmc.sdspi - that Kconfig option also
# auto-selects middleware.sdmmc.common (fsl_sdmmc_common.c), which this
# project doesn't need (fsl_sdspi.c only includes fsl_sdspi.h +
# fsl_sdmmc_spec.h, never fsl_sdmmc_common.h) and can't even compile here:
# fsl_sdmmc_common.h unconditionally #includes fsl_sdmmc_host.h, which only
# exists when a host-controller component (SDHC/USDHC) is also selected -
# this project has no such controller, it talks to the card over LPSPI1
# only. Pulling just the 3 files fsl_sdspi.c actually needs sidesteps that
# entirely.
mcux_add_source(
    BASE_PATH ${SdkRootDirPath}
    SOURCES middleware/sdmmc/sdspi/fsl_sdspi.c
            middleware/sdmmc/sdspi/fsl_sdspi.h
            middleware/sdmmc/common/fsl_sdmmc_spec.h
)
mcux_add_include(
    BASE_PATH ${SdkRootDirPath}
    INCLUDES middleware/sdmmc/sdspi
             middleware/sdmmc/common
)

# FatFs core (ff.c/ff.h) pulled directly, NOT via the SDK's own
# middleware/fatfs Kconfig component - that component's CMakeLists.txt
# always bundles middleware/fatfs/source/diskio.c alongside ff.c whenever
# CONFIG_MCUX_COMPONENT_middleware.fatfs=y, and diskio.c's disk_initialize/
# status/read/write/ioctl would collide at link time with the ones this
# project supplies itself in source/storage/sd_spi_disk.c (which talk to
# LPSPI1 directly - see that file's header comment for why the SDK's own
# SD-over-SPI diskio glue, fsl_sdspi_disk.c, can't be used as-is: it's
# hardcoded to the DSPI peripheral, which doesn't exist on this chip).
# ffsystem.c/ffunicode.c are NOT needed - both are only required for
# features this project's ffconf.h turns off (LFN, FS_REENTRANT).
mcux_add_source(
    BASE_PATH ${SdkRootDirPath}
    SOURCES middleware/fatfs/source/ff.c
            middleware/fatfs/source/ff.h
            middleware/fatfs/source/diskio.h
)
mcux_add_include(
    BASE_PATH ${SdkRootDirPath}
    INCLUDES middleware/fatfs/source
)

if(NOT AI_MODEL_USE_NPU)
# Edge Impulse "C++ library" export (Face_Detection_NXP project, FOMO,
# single class `face`). NOT using the export's own CMakeLists.txt
# (add_subdirectory) - it hardcodes `if(NOT TARGET app) message(FATAL_ERROR
# ...)`, but this SDK's west (non-find_package) build mode names its real
# target ${MCUX_SDK_PROJECT_NAME} (e.g. camera_ai_demo_cm33_core0), not
# literally "app" - add_executable() ALIASing doesn't apply here either
# (CMake only allows ALIAS for library targets). Instead, replicate the
# export's own glob logic (see source/ai/edge_impulse/edge-impulse-sdk/
# cmake/zephyr/CMakeLists.txt, the "arm" architecture branch) directly via
# this project's own mcux_add_source()/mcux_add_include(), which target
# ${MCUX_SDK_PROJECT_NAME} correctly.
set(EI_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/source/ai/edge_impulse)
file(GLOB_RECURSE EI_SOURCE_FILES
    ${EI_SDK_ROOT}/edge-impulse-sdk/*.cpp
    ${EI_SDK_ROOT}/edge-impulse-sdk/*.cc
    ${EI_SDK_ROOT}/edge-impulse-sdk/*.c
    ${EI_SDK_ROOT}/edge-impulse-sdk/*.s
    ${EI_SDK_ROOT}/tflite-model/*.cpp
)
list(REMOVE_DUPLICATES EI_SOURCE_FILES)

# mcux_add_source() always joins BASE_PATH onto every SOURCES entry (even
# already-absolute ones) - convert the glob's absolute paths to relative
# first, matching how every other mcux_add_source() call in this file
# works.
set(EI_SOURCE_FILES_REL "")
foreach(_ei_src ${EI_SOURCE_FILES})
    file(RELATIVE_PATH _ei_src_rel ${CMAKE_CURRENT_SOURCE_DIR} ${_ei_src})
    list(APPEND EI_SOURCE_FILES_REL ${_ei_src_rel})
endforeach()

mcux_add_source(
    BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    SOURCES ${EI_SOURCE_FILES_REL}
)

# The whole project builds with -Werror (see the -O0 comment further down)
# - that's appropriate for our own code, but Edge Impulse's vendored
# TFLite-Micro/CMSIS-NN sources aren't warning-clean under this toolchain's
# -Wall (e.g. a sign-compare in tensorflow/lite/micro/kernels/complex_abs.cc)
# and we don't control that code, so exempt just these files instead of
# disabling -Werror project-wide.
set_source_files_properties(${EI_SOURCE_FILES_REL} PROPERTIES COMPILE_OPTIONS "-Wno-error")

mcux_add_include(
    BASE_PATH ${CMAKE_CURRENT_SOURCE_DIR}
    INCLUDES source/ai/edge_impulse
             source/ai/edge_impulse/edge-impulse-sdk
)

# Needed by the SDK's CMSIS-NN kernels (edge-impulse-sdk/CMSIS/NN/) - same
# defines the export's own zephyr/CMakeLists.txt sets for ARCH=arm.
# SILENCE_EI_CLASSFIER_OBJECT_DETECTION_COUNT_WARNING avoids a #warning (->
# error under -Werror) in model-parameters/model_metadata.h that would
# otherwise also hit model_runner.cpp (one of our own -Werror-strict files).
# EI_PORTING_CLIB=1 selects the generic-libc porting backend
# (edge-impulse-sdk/porting/clib/ei_classifier_porting.cpp, which provides
# ei_malloc/ei_calloc/ei_free/ei_printf/ei_printf_float/etc as weak plain-C
# wrappers) - ei_classifier_porting.h auto-detects Zephyr/Arduino/ESP32/
# Mbed/POSIX/etc by macro, but has no bare-metal-MCU fallback, and every
# porting/*/ei_classifier_porting.cpp is entirely `#if EI_PORTING_<X> == 1`
# - without this, ALL of them compile to empty translation units and the
# link fails with "undefined reference to ei_printf" etc.
# EI_C_LINKAGE=1 is what actually made source/ai/ei_sramx_alloc.c and
# ei_debug_porting.c's overrides take effect: ei_classifier_porting.h only
# wraps ei_malloc/ei_calloc/ei_free/ei_printf/ei_printf_float in
# `extern "C" {}` when this is set (`#if defined(__cplusplus) &&
# EI_C_LINKAGE == 1`). Without it, C++ callers (all the .cpp/.cc SDK
# sources) expect C++-name-mangled symbols, but our overrides are plain .c
# files (always C linkage) - so nothing actually matched, both sets of
# symbols coexisted under different mangled names, and the weak
# C++-linkage clib version (real malloc()/calloc() against the nearly-full
# m_data heap) silently "won" instead of being overridden - the build
# link succeeded either way (no missing symbol), but at runtime
# tflite_learn_..._init()'s single big ei_aligned_calloc() call kept
# failing (EI_IMPULSE_TFLITE_ARENA_ALLOC_FAILED, error -6) with our sramx
# pool's high-water mark stuck at 0 - it was never actually being called.
# mcux_add_macro()'s C++ category is "CX", not "CXX" - CC alone only
# affects .c files, not the .cc/.cpp EI sources or model_runner.cpp.
mcux_add_macro(
    CC "-DEI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=1 -DARM_MATH_LOOPUNROLL -DSILENCE_EI_CLASSFIER_OBJECT_DETECTION_COUNT_WARNING=1 -DEI_PORTING_CLIB=1 -DEI_C_LINKAGE=1"
    CX "-DEI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=1 -DARM_MATH_LOOPUNROLL -DSILENCE_EI_CLASSFIER_OBJECT_DETECTION_COUNT_WARNING=1 -DEI_PORTING_CLIB=1 -DEI_C_LINKAGE=1"
)

else() # AI_MODEL_USE_NPU

# Neutron NPU path (model_runner_npu.cpp) - talks to NXP's own
# middleware/eiq/tensorflow-lite TFLM tree directly, NOT the Edge
# Impulse-vendored copy above (the two must not both be built into the
# same image - different TFLM snapshots, would risk ODR violations/symbol
# clashes if both were linked in). Precompiled lib/cm33/armgcc/libtflm.a
# covers the whole TFLM core + CMSIS-NN reference kernels; the Neutron
# custom-op kernel (tensorflow/lite/micro/kernels/neutron/neutron.cpp)
# isn't part of that prebuilt lib (see middleware/eiq/tensorflow-lite/
# CMakeLists.txt's own "neutron" component) so it's compiled from source
# here, same as micro_time.cpp (platform timer glue - also not in the
# prebuilt lib, per that same CMakeLists.txt always building it from
# source regardless of the "binary" component).
set(TFLM_ROOT ${SdkRootDirPath}/middleware/eiq/tensorflow-lite)
set(NEUTRON_ROOT ${SdkRootDirPath}/middleware/eiq/neutron)

mcux_add_source(
    BASE_PATH ${TFLM_ROOT}
    SOURCES tensorflow/lite/micro/kernels/neutron/neutron.cpp
            tensorflow/lite/micro/kernels/neutron/neutron.h
            tensorflow/lite/micro/micro_time.cpp
            tensorflow/lite/micro/debug_log.cpp
)

# Same reasoning as the Edge Impulse "-Wno-error" exemption above - this
# is vendored NXP code, not warning-clean under this toolchain's -Wall,
# and the whole project otherwise builds -Werror.
set_source_files_properties(
    ${TFLM_ROOT}/tensorflow/lite/micro/kernels/neutron/neutron.cpp
    ${TFLM_ROOT}/tensorflow/lite/micro/micro_time.cpp
    ${TFLM_ROOT}/tensorflow/lite/micro/debug_log.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-error"
)

# BASE_PATH required here - mcux_add_include() silently prepends
# CMAKE_CURRENT_LIST_DIR onto every INCLUDES entry when BASE_PATH is
# omitted, which mangles already-absolute paths like TFLM_ROOT (see the
# "So source/main.c can #include..." comment further down for the same
# gotcha noted for a different call).
mcux_add_include(
    BASE_PATH ${SdkRootDirPath}
    INCLUDES middleware/eiq/tensorflow-lite
             middleware/eiq/tensorflow-lite/third_party/flatbuffers/include
             middleware/eiq/tensorflow-lite/third_party/gemmlowp
             middleware/eiq/tensorflow-lite/third_party/ruy
             middleware/eiq/neutron/driver/include
             middleware/eiq/neutron/common/include
)

# TF_LITE_STATIC_MEMORY/KERNELS_OPTIMIZED_FOR_SPEED match
# middleware/eiq/tensorflow-lite/CMakeLists.txt's own flags for this
# component - needed for the prebuilt libtflm.a's ABI to line up with
# what neutron.cpp (built from source here) expects.
mcux_add_macro(
    CC "-DTF_LITE_STATIC_MEMORY -DKERNELS_OPTIMIZED_FOR_SPEED"
    CX "-DTF_LITE_STATIC_MEMORY -DKERNELS_OPTIMIZED_FOR_SPEED"
)

# libtflm.a: precompiled TFLM core for cm33/armgcc (no source needed/
# available for most of it - see middleware/eiq/tensorflow-lite/
# CMakeLists.txt's "binary" component, same file NXP's own mcxn947 NPU
# examples use). libNeutronDriver.a/libNeutronFirmware.a: the Neutron
# coprocessor's driver + firmware blob for the "mcxn" target family
# (covers MCXN947) - neutronInit() (called internally by neutron.cpp on
# first Prepare(), no manual call needed here) needs both.
target_link_libraries(${MCUX_SDK_PROJECT_NAME} PRIVATE
    ${TFLM_ROOT}/lib/cm33/armgcc/libtflm.a
    ${NEUTRON_ROOT}/mcxn/libNeutronDriver.a
    ${NEUTRON_ROOT}/mcxn/libNeutronFirmware.a
)

endif() # AI_MODEL_USE_NPU

# The whole SDK build defaults to -O0 (west's "debug" CMAKE_BUILD_TYPE,
# not overridable per-app without touching SDK-wide config - see
# WORKLOG.md). At -O0 every GPIO_PinWrite()/GPIO_PortSet() call in the LCD
# bit-bang driver is a real function call (SDK headers mark them
# "static inline", but -O0 mostly ignores that), so per-byte overhead is
# dominated by call/return overhead, not the actual register write.
#
# -O2 here (scoped to just this one file) was tried and confirmed WRONG on
# real hardware - the image came out glitched/noisy, i.e. with
# LCD_BITBANG_DELAY_US=0 (lcd_bitbang.c) the panel's bus timing margin
# comes entirely from GPIO_PinWrite()/GPIO_PortSet() instruction latency,
# and -O2's inlining made those too fast for this panel to latch data
# correctly. Left at -O0 (the SDK-wide default) instead - do not re-enable
# without also raising LCD_BITBANG_DELAY_US back off 0 to compensate, and
# re-test on hardware either way.
# if(LCD_ARDUINO_HEADER_BITBANG OR LCD_BITBANG_DIAGNOSTIC)
#     set_source_files_properties(
#         ${CMAKE_CURRENT_SOURCE_DIR}/source/display/lcd_bitbang.c
#         PROPERTIES COMPILE_OPTIONS "-O2"
#     )
# endif()
#
# lcd_spi_hw.c (Arduino header's hardware-SPI driver) is a DIFFERENT case
# from the -O2-is-wrong reasoning right above, and IS built at -O2 here:
# its bus timing comes entirely from the LPSPI peripheral's own hardware
# baud-rate generator (see lcd_spi_hw.c's LCD_SPI_BAUDRATE_HZ), not from
# software instruction latency the way the GPIO bit-bang driver's was - so
# -O2 here can only speed up CPU-side work (LCD_PushPixelsOpen()'s
# byte-swap loop), it cannot make the electrical SPI signal itself
# "too fast" the way it did for lcd_bitbang.c. Part of the same 2026-09-04
# fps fix as the LPSPI1 clock-source change in hardware_init.c and the
# pixel-batching rewrite in lcd_spi_hw.c itself - see WORKLOG.md.
if(LCD_ARDUINO_HEADER_BITBANG)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/source/display/lcd_spi_hw.c
        PROPERTIES COMPILE_OPTIONS "-O2"
    )
endif()

# So source/main.c can #include "camera_capture.h" etc. by simple name.
# NOTE: mcux_add_include() joins INCLUDES onto CMAKE_CURRENT_LIST_DIR itself
# when no BASE_PATH is given, so these must be relative (an absolute path
# here silently doubles into a nonexistent "<dir>/<dir>/source" path).
mcux_add_include(
    INCLUDES source
             source/camera
             source/display
             source/ai
             source/usb
             source/storage
)

# Produce a raw .bin next to the .elf so pyOCD can flash it directly.
mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)
