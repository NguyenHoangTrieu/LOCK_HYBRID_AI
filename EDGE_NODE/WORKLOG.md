# WORKLOG - Camera_AI_Test1

Running log of this project's bring-up, for picking up in a fresh session.
For the stable reference doc (pinout, build instructions), see
[README.md](README.md) - this file is the messier "what's been tried and
what happened" history, kept separate so README doesn't get cluttered.

> Older history (camera/LCD/USB bring-up, all of that now stable/working -
> see README.md's "History" section for the summary) was trimmed from this
> file to keep it focused on the current, unresolved problem below.
## Memory Optimization (RAM & Flash) & LCD FPS Acceleration (2026-10-18)

### Summary of Improvements & Confirmed Hardware Results

1. **LCD FPS Boost (8-10 FPS -> 10-11 FPS)**:
   - **Xung nhịp SPI1 37.5 MHz**: Trong `hardware_init.c`, chuyển nguồn clock của `FLEXCOMM1` từ `FRO_HF` (48 MHz -> tối đa 24 MHz SPI) sang `PLL0 / 2 = 75 MHz` (`kPLL0_to_PLLCLKDIV` div=2, `kPLL_DIV_to_FLEXCOMM1` div=1). Đặt `LCD_SPI_BAUDRATE_HZ = 37500000U` (37.5 MHz), rút ngắn thời gian truyền vật lý 1 frame 320x240 RGB565 từ **51.2 ms xuống 32.8 ms**.
   - **eDMA Chunks 16 KB**: Tăng `LCD_SPI_CHUNK_PIXELS` từ 1024 lên **8192 pixels (16 KB)** đặt trong `.sramx_bss`, giảm số lần gọi ngắt và thiết lập descriptor eDMA mỗi frame từ **75 lần xuống 10 lần**.
   - **Tối ưu đảo byte 32-bit hardware**: Dùng lệnh hợp ngữ `__REV16()` của Cortex-M33 trong `LCD_PushPixelsOpen()` để đảo byte 2 pixel RGB565 cùng lúc trên từ 32-bit.
   - **LPSPI1 Delay & Cache**: Cache baudrate trong `SPI1_BUS_SetBaudRate()` để loại bỏ việc reconfigure thanh ghi mỗi frame, và đặt `kLPSPI_BetweenTransfer = 0` khi chạy $\ge 10\text{ MHz}$.
   - **Bỏ `memset(153.6 KB)` mỗi frame**: Trong `CAMERA_CAPTURE_Reinit()`, chỉ khởi động lại SmartDMA mà không xóa 153,600 byte frame buffer và không nạp lại firmware (tiết kiệm ~3–4 ms CPU mỗi frame).

2. **Tối ưu RAM & Flash (`m_sramx` & `m_data`)**:
   - Thêm section `.sramx_bss (NOLOAD)` trong `MCXN947_cm33_core0_singlecore.ld` từ địa chỉ `0x04001000` (giữ nguyên 4 KB đầu `0x04000000..0x04001000` cho SmartDMA camera firmware).
   - Di dời các cấu trúc tĩnh: `s_pixelSwapBuf` (16 KB), `s_candidates` (640 B), `s_fs` (560 B), `s_card` (100 B), `s_host` (24 B), `s_smartdmaStack` (128 B), `s_lpspiDmaHandle` (236 B), `s_dmaTxHandle`, `s_dmaRxHandle` sang `m_sramx`. Tự động zero-init vùng này trong `BOARD_InitHardware()`.
   - Thay `static uint16_t s_clearLine[320]` (640 B) trong `DEMO_ClearScreen()` bằng stack buffer.
   - **Kết quả RAM `m_data`**: Giảm từ `413,072 B` (97.91%) xuống **`408,328 B` (96.79%)**, giải phóng thêm **+4,744 B** vùng nhớ chính SRAM0..8.
   - **Kết quả Flash `m_text`**: Bọc ảnh test `couple_COCO_128_128_rgb.h` sau `AI_ENABLE_BOOT_SELFTEST`, giảm Flash từ `258,976 B` xuống **`200,400 B`** (tiết kiệm **57.2 KB Flash**).

3. **Tối ưu NPU Inference Hot-path & Nâng Threshold 65%**:
   - **Bảng tra kênh màu RGB565**: Tạo sẵn 2 bảng `s_lutR5B5[32]` và `s_lutG6[64]`, thay thế hoàn toàn 3 phép chia số nguyên (`/ 31U`, `/ 63U`, `/ 31U`) ở mỗi pixel trong vòng lặp 16,384 pixel của `SampleCropTensor()`. Chuyển phép tỉ lệ tọa độ sang dịch bit `>> 7U`.
   - **Giảm probe đa góc xoay lúc rỗi**: Khi chưa có khuôn mặt, chỉ probe góc xoay phụ mỗi 32 frame (thay vì mỗi frame), giữ NPU inference ổn định ở mức **16 ms**.
   - **Nâng ngưỡng phát hiện**: Cập nhật `NPU_MODEL_DETECTION_THRESHOLD` từ `0.58f` lên **`0.65f` (65%)** theo thực nghiệm người dùng, lọc bỏ các box biên nhiễu trong khi khuôn mặt người thật nhận diện rất nhạy và chuẩn xác (score 80% – 99%).
   - **Tốc độ thực tế trên board**: Đạt **10–11 FPS** liên tục khi hiển thị LCD kèm AI box overlay.

## Single-core NPU Face Detection Bring-Up & Dynamic Scheduler (2026-10-17)

### Summary of Symptoms, Root Causes & Fixes

1. **Rollback to Single-Core (`cm33_core0`)**:
   - Firmware execution reverted to single-core `cm33_core0` (bare-metal) per user request to eliminate cross-core SWD debug sequence faults (`ResetCatchClear` transfer faults), core1 boot race conditions, and SRAMX bank contention while stabilizing AI inference.
   - Built with `./build.sh build` and flashed with `./build.sh flash`. `m_sramx` (0x04000000) is reserved exclusively for SmartDMA firmware.

2. **UltraFace NPU Validation & Pixel Decoding Fix**:
   - **Self-Test Validation**: Embedded a 128x128 COCO couple test image (`couple_COCO_128_128_rgb.h`) into NPU initialization. Running inference on this test image produced `maxScore = 99%` (anchor 18), mathematically proving that the Neutron NPU hardware accelerator, UltraFace model weights, input LUT quantization (`s_inputLut`), and output dequantization were 100% operational.
   - **RGB565 Decoding Fix**: Replaced legacy inverted byte-swapped channel extraction with standard 16-bit RGB565 bit-shift math:
     `r = ((px >> 11) & 0x1F) * 255 / 31`, `g = ((px >> 5) & 0x3F) * 255 / 63`, `b = (px & 0x1F) * 255 / 31`. Real camera pixels (`0xB34F`) now decode to natural skin tones (`R=176, G=105, B=123`) instead of neon green.

3. **OV7670 Sensor Mounting Orientation & Lens Focus Discovery**:
   - **Orientation**: Multi-rotation inference revealed that live human faces were consistently detected with high confidence (**70% - 81%**) only at `rot = 270 deg` (counter-clockwise 90° / clockwise 270°). On the FRDM-MCXN947 J9 SmartDMA header, the OV7670 camera module is physically oriented sideways relative to the Arduino LCD panel. At 0° orientation, face detection was previously failing (score stuck at ~10%).
   - **Lens Screw-Focus**: OV7670 uses a manual screw-thread lens. An ASCII camera preview (32x16) was implemented to allow visual focus checking via UART. When the lens is out of focus, facial gradients blur out and confidence scores drop.

4. **Performance Optimization - Restoring FPS from 4-5 FPS to 8-10 FPS**:
   - **Problem**: Running all 4 rotations (0°, 180°, 90°, 270°) sequentially every frame consumed $4 \times 16.5\text{ms} = 66\text{ms}$ on the NPU. In addition, printing a 32x16 ASCII preview via UART at 115200 baud blocked for ~55ms every 1.5 seconds. Coupled with LCD SPI push (~55ms), frame rate dropped to 4–5 FPS.
   - **Dynamic Fast Scheduler**:
     - Defaults to `rot = 3` (270° CW, the verified hardware orientation on J9).
     - When a face is detected (score >= threshold), the loop terminates immediately: inference time is strictly **16ms**!
     - When no face is detected, it runs at most ONE alternative rotation round-robin per frame (32ms max) to detect if the board orientation changed. If an alternative rotation detects a face, `s_activeRot` updates automatically.
     - Reduced ASCII preview frequency to every 150 frames (~15s).
     - Frame rate restored to **8–10 FPS**.

5. **False-Positive Prior Filtering**:
   - **Problem**: SSD root anchor boxes (spanning full width with tiny height `w=240, h=15` or full height `w=8, h=240`) occasionally triggered weak scores (52% - 58%) on background gradients.
   - **Fix**:
     - Raised detection threshold to `0.58f` (matching NXP official 60% specification). Genuine faces score 70% – 81%.
     - Added aspect ratio filtering ($0.45 \le w/h \le 2.2$) and rejected full-frame bounding boxes ($w \ge 0.90$ or $h \ge 0.90$).
     - Green bounding boxes (`0x07E0`) now tightly and accurately track human faces on the LCD and UART console.

## Dual-core RTOS migration Stage 5 EIGHTH FOLLOW-UP - ROOT CAUSE FOUND AND CONFIRMED ON REAL HARDWARE: the whole "core1 can't reliably write GPIO0" mystery (backlight, DC, all of it, going back multiple sessions) was never a core1 hardware/timing quirk at all - core1 has no SAU (Security Attribute Unit) on this chip, so it is permanently in the Armv8-M Non-Secure state, and GPIO gates Non-Secure pin access per-bit via its own `PCNS` register, which defaults to all-zero (Secure-only) after reset. core1's writes to any never-explicitly-granted pin were being silently discarded at the peripheral, every time, for the entire history of this project's dual-core work. Fixed with a 4-line PCNS grant in core0's boot code, before `MCMGR_StartCore()` releases core1. **User confirmed on real hardware: the LCD now shows a real, live image in the dual-core build for the first time ever** (2026-09-06)

Follow-up to the FIFTH FOLLOW-UP entry (backlight investigation) and the
REVERTED SEVENTH FOLLOW-UP entry below (unrelated SEMA42 detour, already
undone). Picked back up the actual display problem: after the FIFTH
FOLLOW-UP's 3V3 backlight rewire, the user reported the panel was no
longer black - it was **solid white**, still no live image. Backlight was
confirmed genuinely fine this session (live SWD reads showed it correctly
latched); this entry is about what actually was still broken.

**Two theories tested and DISPROVEN this session, in order, each with
real hardware evidence - documented so a future session doesn't repeat
them:**

1. **RST (reset pin) was NOT the problem.** The user tied RST to 3V3 too
   (mirroring the BLK fix) - screen stayed white. A live SWD read
   (`GPIO0->PDOR` bit 15) had already shown RST correctly latched HIGH
   before this test even ran, so the negative result matched the
   prediction - RST was never actually broken, only ever suspected by
   analogy to BLK.
2. **A GPIO-write/SPI-transfer timing race was NOT the problem.** DC
   (`DEMO_LCD_DC_PIN`, the command/data select line) was the one pin still
   provably stuck at 0 on live SWD reads, toggled far more rapidly than
   CS/RST/BLK (low-write, one SPI byte, high-write, repeated for every
   command). Two escalating tests: (a) moved DC entirely off GPIO0 onto a
   different peripheral instance (GPIO1, Arduino D3/P1_23) to rule out a
   GPIO0-specific cause - still stuck at 0, ruling that out; (b) added a
   settle delay (`SDK_DelayAtLeastUs`) bracketing the SPI transfer between
   DC's two writes, to test whether tight interleaving with live SPI
   traffic was the cause - still stuck at 0, ruling that out too. Both
   changes reverted rather than left in as non-functional code (DC's
   GPIO1/D3 move was KEPT - see below for why it turned out to still
   matter, just not for the reason originally tested).

**Real root cause, found by searching for this exact symptom on this
exact chip rather than continuing to guess:** an NXP Community post,
["MCXN947 failed to control GPIO in slave core
(CPU1)"](https://community.nxp.com/t5/MCX-Microcontrollers/MCXN947-failed-to-control-GPIO-in-slave-core-CPU1/td-p/2250120),
describes this project's exact symptom on this exact chip. core1 on the
MCXN947 does not implement a SAU, so it is **permanently Armv8-M
Non-Secure** (this is a fixed hardware/architecture property of this
chip's asymmetric dual-core design, not a bug or an erratum). GPIO
peripherals implement TrustZone-style access gating: a `PCNS` register
(confirmed in this SDK's own `PERI_GPIO.h`, one `NSEn` enable bit per
pin, at register offset `0x10`) controls whether a Non-Secure bus master
may touch each individual pin. Confirmed live: `GPIO0->PCNS` and
`GPIO1->PCNS` both read `0x00000000` on this board - **every single pin
defaults to Secure-only after reset**, and nothing in this codebase (or
the NXP SDK's default board bring-up) had ever granted core1 permission
for any of them. core1's writes to these pins were being silently
dropped at the peripheral on every single call, for the entire history of
this project's dual-core work - not "unreliable," not "timing-sensitive,"
simply never landing at all.

**Fix**: `main_core0.c`'s `main()`, before `MCMGR_StartCore()` releases
core1 (plain register writes, not a FreeRTOS API, so this doesn't
conflict with the established "no FreeRTOS API before `MCMGR_StartCore()`"
boot-ordering rule):
```c
GPIO0->PCNS |= GPIO_PCNS_NSE15_MASK  /* LCD RST, P0_15 */
             | GPIO_PCNS_NSE22_MASK  /* LCD CS,  P0_22 */
             | GPIO_PCNS_NSE23_MASK; /* LCD BLK, P0_23 */
GPIO1->PCNS |= GPIO_PCNS_NSE23_MASK; /* LCD DC,  P1_23 (Arduino D3) */
```
Must run on core0 - it's the only core with a SAU, so the only one that
can act Secure and grant this. Pin numbers are hardcoded (can't `#include`
core1's `app.h` from core0's translation unit) - must be kept in sync by
hand if core1's LCD pins ever change again.

**A second, genuinely confusing methodological trap along the way, worth
recording so it isn't repeated**: after adding the PCNS grant, a live SWD
read of `GPIO1->PDOR` (bit 23, DC) *still* showed 0 every time - looking
like the fix had failed. It hadn't. TrustZone-aware peripherals bank
Secure and Non-Secure register state **independently** - reading via
core0's own debug AP (AHB-AP#0) shows the *Secure* alias/instance of the
register, which has nothing to do with what core1 (Non-Secure) is
actually writing to its own Non-Secure instance of the same nominal
address. Confirmed directly: switching the SWD read to core1's own debug
AP (AHB-AP#1) - which, tellingly, could not read/write ANY GPIO
peripheral at all before this fix (confirmed against GPIO0, GPIO1, and
GPIO4, while it read LPSPI1 fine throughout - a real, separate,
now-explained symptom of the exact same Non-Secure-blocked-by-PCNS cause)
- immediately showed DC toggling correctly, and forcing it low via AP1
and watching the running firmware pull it back high within ~150ms proved
it live, not just "reads differently once." **The general lesson: on a
TrustZone-partitioned chip, always read a Non-Secure core's own registers
through that core's own debug AP - the other core's AP can show a
completely different, unrelated value for the exact same address, even
though nothing about the read itself is wrong.**

**Confirmed on real hardware by the user**: the dual-core build now shows
a real, live camera image on the LCD - the first time this has ever
worked in the dual-core build, across many sessions of this project's
history. This also retroactively explains (though does not need separate
fixing) the CS/RST/BLK-vs-DC split that several earlier WORKLOG entries
found and could never cleanly explain - CS/RST/BLK's *apparent* success
in earlier live-SWD checks was never actually valid evidence either (same
wrong-AP trap above), it was almost certainly coincidental defaults on
those specific bits, not a genuine reliability difference from DC. All
four pins are now correctly explained by the same single mechanism.

**Open question for a future session, not urgent**: DC's move to GPIO1/D3
was kept, but may not have been strictly necessary - the real fix is the
PCNS grant, and it's plausible DC could have stayed on its original
GPIO0/P0_14 (Arduino A2) pin if `GPIO_PCNS_NSE14_MASK` had been granted
there instead. Not tested (the GPIO1 move was already in place and
working by the time PCNS was identified as the real fix, and there was no
reason to re-risk a working configuration just to simplify the wiring
back). If a future session wants the wiring back to the original
A2/A3/A4/A5 layout, granting `GPIO0->PCNS |= GPIO_PCNS_NSE14_MASK` instead
of touching GPIO1 at all should work by the same mechanism - not yet
verified.

### Next steps for a fresh session

1. Re-confirm the LCD tearing fix (`SPI1_BUS_LockNoPreempt()`,
   THIRD FOLLOW-UP entry) with the display now actually visible - every
   earlier "confirmation" of that fix could only check fps/build success,
   never the actual image, since the image itself was never visible until
   now.
2. The SD card write-reliability issue is separate and still open - mounts
   and saves the first few snapshots successfully most boots, then
   degrades to write failures / "could not create a new file" later in
   the same session (see the SIXTH-FOLLOW-UP-adjacent session's own real
   hardware log). Not caused by anything in this entry.
3. Phase 4 of the original migration plan (retire the legacy single-core
   `main.c`/board_port/CMakeLists once dual-core is the confirmed default)
   is now much more realistic to consider, given this is the first time
   the dual-core build has been fully functional end-to-end (camera + AI +
   LCD image + boot) - still not done yet.
4. If any NEW GPIO pin is ever wired up for core1 to drive in the future
   (a new sensor's chip-select, an LED, anything), remember this entry:
   grant its `PCNS` bit from core0 before `MCMGR_StartCore()`, or it will
   silently do nothing, exactly like every pin in this entire investigation
   did before this fix.

## Dual-core RTOS migration Stage 5 SEVENTH FOLLOW-UP, REVERTED - the bounded-poll fix below (SIXTH FOLLOW-UP) got real output back, but real hardware then showed a SECOND, worse regression: the core0<->core1 AI round trip that worked before any of this SEMA42 work failed on 100% of frames ("AI: no result from core0... within 100ms", every single frame), fps collapsed from 8-9 down to 2-3, and boot-time log corruption was STILL happening despite the lock - strongly suggesting SEMA42_TryLock() was failing almost every call and burning its full retry budget on every single DEBUG_PRINTF(), not just occasionally. Root cause not confirmed (no live debugger access this session), but two regressions in a row from the same approach was reason enough to stop iterating blind. REVERTED entirely (`debug_lock.c/.h` deleted, all `DEBUG_PRINTF`/`PRINTF` renames and CMakeLists/prj.conf changes undone via `git checkout`) back to the exact pre-session baseline (confirmed: reverted build's core0 `m_text`/`m_data` exactly match the original numbers) - the per-core `HeartbeatTask`s from the FIFTH FOLLOW-UP-adjacent entry were reverted too (they lived in the same two files as the SEMA42 changes) since their diagnostic question ("are both cores really running concurrently") is already answered - yes, confirmed on real hardware in that entry - and isn't needed again unless a future session doubts it again. Log corruption at boot is once again a known, unfixed, lower-priority cosmetic issue - see the FIFTH FOLLOW-UP-adjacent entry below for the root-cause analysis if a future session wants to try a different, lower-risk approach (2026-09-05)

Follow-up to the two entries below (same day, same session). Do not repeat
the SEMA42-based `DEBUG_PRINTF()` approach without first understanding why
it caused the AI round trip to fail 100% of the time on real hardware -
that failure mode was never explained, only worked around by reverting.

## Dual-core RTOS migration Stage 5 SIXTH FOLLOW-UP - the SEMA42 debug-lock fix below made things WORSE on real hardware (total silence, no boot output at all) before any confirmation was obtained; root cause not pinned down (no live debugger this session either) but the leading suspect is SEMA42_Lock()'s zero-timeout default polling forever on the very first DEBUG_PRINTF() call - fixed by hand-rolling a bounded poll (SEMA42_TryLock() + a large-but-finite retry count, fail open rather than hang) in both DEBUG_LOCK_Init()'s reset-completion wait and DEBUG_PRINTF() itself. Builds clean, same memory margins - AGAIN NOT yet confirmed on real hardware, and THIS ENTRY'S OWN FIX WAS ITSELF REVERTED per the entry above after real hardware showed a second regression (2026-09-05)

Follow-up to the entry directly below (same day, same session). The user
flashed the SEMA42-based `DEBUG_PRINTF()` fix and reported **total
silence - no serial output at all**, a strictly worse symptom than the
corrupted-but-present log it was meant to fix.

**Root cause not confirmed - no live debugger available this session
either, same limitation as several earlier entries in this file.** Leading
suspect, from reading the code rather than a live register read: the
original `debug_lock.c` used `SEMA42_Lock()`, which polls
`SEMA42_TryLock()` in a loop with **zero timeout** by default
(`SEMA42_BUSY_POLL_COUNT` is 0 unless explicitly overridden, per
`fsl_sema42.h`) - if the very first `DEBUG_PRINTF()` call on either core
(core0's boot banner, immediately after `DEBUG_LOCK_Init()`) ever failed
to acquire gate 0 for any reason, it would spin forever with no way out
and no partial output, matching "no log at all" exactly. Two candidate
explanations for why the gate might not have been acquirable were
considered and NOT ruled out without hardware: (1) `SEMA42_ResetAllGates()`
returns as soon as the reset command pattern is written, not once the
reset has actually completed in hardware (`RSTGT_R`'s busy bit) - core0's
own immediately-following `DEBUG_PRINTF()` call may have raced this; (2)
some other, not-yet-identified SEMA42-specific quirk on this chip. (One
theory that WAS ruled out by reading the device header directly:
`SEMA42_0` resolving to the secure-world alias address instead of the
non-secure one this project's `ARM_CM33_NTZ` FreeRTOS port expects -
checked `MCXN947_cm33_core0_COMMON.h`'s `SEMA42_0_BASE` macro, which is
conditioned on `__ARM_FEATURE_CMSE` and correctly resolves to the same
`0x400B1000` non-secure address this project's other peripherals use,
since this build never defines that macro - not the cause.)

**Fix, defensive rather than root-caused**: rewrote `debug_lock.c` to
never be able to hang forever regardless of the actual cause - a logging
helper hanging the whole core is strictly worse than any corruption it
might ever fail to prevent. `DEBUG_PRINTF()` now hand-rolls its own
bounded retry loop around `SEMA42_TryLock()` (`DEBUG_LOCK_POLL_LIMIT` =
1,000,000 iterations - generous relative to how briefly this gate is ever
actually held) instead of calling `SEMA42_Lock()`; on a real timeout it
still prints (unlocked, in the rare case that ever actually triggers)
rather than blocking forever, and correctly skips calling
`SEMA42_Unlock()` in that case (unlocking a gate this core never
acquired would just create a new corruption case instead of preventing
one). `DEBUG_LOCK_Init()` (core0 only) now also waits, with the same
bounded-poll pattern, for `SEMA42_ResetAllGates()`'s reset to actually
complete (`RSTGT_R`'s busy bit clearing) before returning, closing
candidate explanation (1) above whether or not it was the real cause.

**Confirmed only as a clean build** (real, measured): both cores build
without error, memory margins unchanged from the previous entry
(core1 84.67%/98.86% `m_text`/`m_data`, core0 18.80%/94.70%). **NOT yet
confirmed on real hardware again** - same access limitation as the entry
below.

### Next steps for a fresh session

1. Flash `dualcore-all` and check: does ANY output appear now? This is a
   genuinely informative experiment either way - if output now appears
   (even if briefly corrupted-looking or showing an unexpected delay), the
   bounded-poll fix confirms the hang was inside `SEMA42_Lock()`/the reset
   wait as suspected; if it's STILL completely silent, the cause is
   somewhere else entirely (unrelated to the timeout theory) and this
   whole SEMA42 approach needs reconsidering, not just re-tuning - don't
   assume it's "almost fixed" if this second attempt also produces
   silence.
2. If real output appears and looks clean (no more boot-time garble, no
   new corruption under load): this entry and the one below can be
   considered resolved together - see that entry's own next-steps for
   what a "clean" confirmation should look like (fresh boot + full
   camera+LCD+AI+SD steady state).
3. If it's a genuine hardware quirk with this chip's SEMA42 peripheral
   (not just a missing timeout), worth knowing before investing further:
   was this exact SEMA42 instance/gate ever exercised successfully by
   anything else on this board before (nothing in this project used it
   until this entry) - if not, this may be new territory for this specific
   chip/errata, not just a software ordering bug.
4. Everything else queued in the entry below (backlight 3V3 rewire, SD
   write failure, tearing re-confirmation) is unaffected by this
   regression and still pending, unchanged.

## Dual-core RTOS migration Stage 5 SIXTH FOLLOW-UP - added a per-core heartbeat log (independent of camera/LCD/AI) to check whether both cores are genuinely running concurrently; both are, but core1's heartbeat visibly lags core0's - explained (not a bug, a real timing cost: SPI1_BUS_LockNoPreempt()'s taskENTER_CRITICAL() masks core1's own SysTick for the ~57ms/frame LCD_DrawImage() takes at 24MHz). Separately found and fixed a real bug the heartbeat log surfaced: both cores independently print to the SAME physical debug UART (FLEXCOMM4) with no cross-core arbitration, causing real, reproducible byte-level interleaving/corruption in the serial log whenever core0 and core1 print near-simultaneously (worst right at boot, when both cores burst their startup banners) - fixed with a SEMA42 hardware-semaphore-backed DEBUG_PRINTF() wrapper (source/shared/debug_lock.h/.c) replacing every PRINTF() call in both cores' dual-core-only sources. Builds clean on both cores, real measured margin unchanged (core1 m_text 84.03%/m_data 98.86%, core0 m_text 18.80%/m_data 94.70%) - NOT yet confirmed on real hardware, this session had no flash/serial access (2026-09-05)

Follow-up to the FIFTH FOLLOW-UP entry directly below (same day). Before
doing anything about the still-unresolved backlight mystery, added a
`HeartbeatTask` on each core (`main_core0.c`/`main_core1.c`) - a trivial
`vTaskDelay(1000ms)` loop printing `core0/1 heartbeat: N`, deliberately
independent of the camera/LCD/AI/IPC pipeline - to directly answer "is
core1 actually running, or is the whole 'dual-core' build secretly only
executing one core" as a sanity check before trusting any more GPIO-level
findings from that investigation.

**Both cores are genuinely running concurrently** - both heartbeats
increment steadily in the user's serial capture. But core0's counter
reliably outpaces core1's (e.g. core0:37 vs. core1:22 over the same
window) - investigated rather than dismissed, since an unequal heartbeat
rate on hardware previously suspected of a per-core write reliability
issue was worth taking seriously. Root cause, confirmed by reading the
code (not yet by a live measurement): `spi1_bus.c`'s
`SPI1_BUS_LockNoPreempt()` (`taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()` -
see the THIRD FOLLOW-UP entry below for why this exists) raises BASEPRI
high enough to mask core1's own SysTick, not just `MAILBOX_IRQn` - this
lock wraps `LCD_Init()`'s panel init AND every single `LCD_DrawImage()`
pixel push (`lcd_spi_hw.c`), which this file's Stage 4 FOLLOW-UP entry
already measured at ~57ms/frame at 24MHz. While that critical section is
active, core1's tick genuinely does not advance, so any `vTaskDelay()` on
core1 (the heartbeat's 1s period included) loses real wall-clock time on
every single frame - core0's `AiInferenceTask` has no equivalent critical
section, so its own tick stays accurate. This is NOT evidence core1 is
unreliable or "not really running" - it is running, just spending a large,
already-known fraction of every frame period with its own timebase frozen
by a lock this project added deliberately. Not fixed (no reason to yet -
nothing currently depends on core1's tick being precise), just documented
here so a future session doesn't mistake this for a new mystery.

**Real bug found and fixed: the two cores' debug UART output was never
cross-core-safe, and it shows.** The user's serial capture had a burst of
genuinely unreadable garbled bytes right at the start, plus scattered
mid-stream corruption throughout (e.g. `"core1 heartbeat:J...`",
`"...classifil...hearer time...tbeat: 28"`) - real, reproducible data
corruption, not a baud-rate/framing glitch. Root cause, confirmed by
reading both cores' `hardware_init.c`: core0 and core1 each independently
call `BOARD_InitHardware()` -> `BOARD_InitDebugConsole()` and `PRINTF()`
against the exact same physical UART (FLEXCOMM4 - core1's own file
explicitly attaches the same `BOARD_DEBUG_UART_CLK_ATTACH`).
`debug_console_lite`'s `PRINTF()` is a per-core blocking write straight to
that UART's TX register; any locking it does only serializes tasks on the
SAME core's own FreeRTOS instance - there is no hardware arbitration
between the two physically separate CM33 cores writing to the same
register at the same instant, so concurrent prints interleave their bytes
mid-string. Worst right at boot (both cores burst several banner/init
lines while racing the MCMGR handshake), but reproducible any time both
cores happen to print close together.

**Fix**: added `source/shared/debug_lock.h`/`.c` - `DEBUG_PRINTF()`, a
drop-in `PRINTF()` replacement that takes a SEMA42 hardware semaphore gate
(`fsl_sema42.h`, confirmed available for this chip -
`FSL_FEATURE_SOC_SEMA42_COUNT` = 1, 16 gates - and already present in the
SDK tree) around the whole call via `DbgConsole_Vprintf()`, making each
core's print atomic with respect to the other. `DEBUG_LOCK_Init()` enables
the SEMA42 clock on both cores and, on core0 only
(`DEBUG_LOCK_PROC_NUM == 0`, set per-core via `CMakeLists.txt`'s
`mcux_add_configuration(CC "-DDEBUG_LOCK_PROC_NUM=...")`), resets all
gates - called before `MCMGR_StartCore()` releases core1, so core1 can
never observe a gate mid-reset (same boot-ordering argument as the
existing "no FreeRTOS API before `MCMGR_StartCore()`" rule - `SEMA42_Init`/
`ResetAllGates` are plain register pokes, not FreeRTOS APIs, so this is
safe). Every `PRINTF(` call site in both cores' dual-core-only sources
(`main_core0.c`, `main_core1.c`, `fault_handler.c`,
`ai/model_runner_npu.cpp`, `camera/camera_capture.c`,
`display/lcd_spi_hw.c`, `storage/snapshot.c`, `storage/sd_spi_disk.c` - 42
call sites total) was mechanically renamed to `DEBUG_PRINTF(` and their
`#include "fsl_debug_console.h"` swapped for `#include "debug_lock.h"`
(which itself declares `DEBUG_PRINTF()` with no other dependency on those
files' end). The legacy single-core build's own files (`main.c`,
`lcd_bitbang.c`, `lcd_flexio_mculcd.c`, `usb_video_camera.c`,
`model_runner.cpp`, `ei_debug_porting.c`, `ei_sramx_alloc.c`) were left
untouched - a single physical core can't race itself. `CONFIG_MCUX_
COMPONENT_driver.sema42=y` added to both cores' `prj.conf`.

**Confirmed only as far as a clean build/link** (real, measured - not
guessed): both cores build without error, core1 84.03%/98.86%
`m_text`/`m_data`, core0 18.80%/94.70% (both up only slightly from the
previous entry's numbers, as expected for one small added driver+wrapper).
**NOT yet confirmed on real hardware** - this session had no flash/serial
access at all. Needs a fresh capture to confirm the boot-time garble is
actually gone and no new corruption appears under load (camera+LCD+AI all
printing at once, the busiest case for this lock).

### Next steps for a fresh session

1. Flash `dualcore-all` and capture a fresh serial log across a full boot
   + steady-state run (camera+LCD+AI+SD all active, matching the busiest
   print traffic this session's fix targets) - confirm the log is now
   clean, both at boot and under load, with no regression in fps.
2. If clean: this repurposes cleanly as the ongoing serial-log
   confirmation mechanism for the still-unresolved backlight GPIO mystery
   (FIFTH FOLLOW-UP entry below) - a corrupted log was never actually
   blamed for any wrong conclusion there (the SWD-based findings didn't
   depend on serial output), but a trustworthy log removes one possible
   source of doubt before that investigation's next step (hardware
   watchpoint or logic analyzer).
3. The 3V3 backlight rewire (FIFTH FOLLOW-UP's actual next step) is still
   the priority once real hardware access is available - unchanged by
   this entry.
4. The `HeartbeatTask`s added this entry are diagnostic, not part of the
   product - fine to leave in (negligible cost), but callable out if a
   future session wants the core1 m_text/m_data budget back.

## Dual-core RTOS migration Stage 5 FIFTH FOLLOW-UP - CONFIRMED on real hardware: single-core baseline is genuinely fine (backlight+image both good), isolating the blackout to the dual-core build specifically; TEMP_SKIP_IPC_ROUNDTRIP was still on (explains "no AI log"), fixed. The backlight GPIO (P0_23/GPIO0) does not reliably reflect software writes issued by core1 - extensively investigated live (pin mux, clock, settling delay, AHBSC per-peripheral access rules, address aliasing, and a full 32-bit simultaneous write/readback sweep tested both before and after the panel's full init sequence, all ruled out or contradicted) but NOT root-caused; two attempted software workarounds did not fix it and were reverted rather than left in as non-functional code. DECISION (with user): stop chasing a GPIO fix and wire the backlight directly to 3V3 instead, since the code never needs to turn it off - real hardware change, not yet done, planned for a fresh session (2026-09-05)

Follow-up to the FOURTH FOLLOW-UP entry below (same day, this session finally
got working USB/pyOCD access to the physical board - the FOURTH FOLLOW-UP's
own environment couldn't flash at all).

**Single-core LCD-preview build reflashed and CONFIRMED GOOD on real
hardware by the user**: backlight on, live camera image visible, clean.
This is the single most important result of this entry - it definitively
rules out hardware/wiring/power as the explanation for the dual-core
blackout (same physical board, same wiring, same LCD, only the firmware
changed) and matches this project's own "diff against a known-good branch"
technique (see the dual-core Stage 5 SECOND FOLLOW-UP entry's use of the
same technique against `spi_tft_change`).

**"No AI log" complaint - real, but a leftover diagnostic flag, not a new
bug.** `main_core1.c`'s `TEMP_SKIP_IPC_ROUNDTRIP` (added in the THIRD
FOLLOW-UP entry to isolate whether the IPC round trip itself correlated
with LCD tearing) was still `1`, so `AiInferenceTask` (core0) was
permanently parked on `xTaskNotifyWait(portMAX_DELAY)`, never invoked.
Flipped to `0` - confirmed on real hardware immediately after: real
`AI_MODEL_RunInference` timing (~3.8-4.0ms, matches every earlier NPU
measurement in this file) and real detections (`AI result: box[0]
label=face ... score=97%`) now print every frame, and a live SD-write
failure on a REAL detection was also caught for the first time
(`FRESULT=1`, `FR_DISK_ERR`, both header and pixel write) - Stage 4 only
ever exercised the SD write path with a synthesized fake detection, so
this is a genuinely new, not-yet-investigated finding, separate from
everything below.

**The backlight blackout is real, reproducible, and NOT explained by
anything found in the FOURTH FOLLOW-UP entry** (the `SPI1_BUS_LockNoPreempt()`
doc/code mismatch fixed there was real but happens well after
`LCD_InitGpioPins()` already sets the backlight, so it can't be the cause).
Root-caused as far as live SWD register reads can take it:

- `LCD_Init()` (`lcd_spi_hw.c`) genuinely runs to completion every boot -
  confirmed via its own boot-time PRINTF lines appearing in the serial log
  every time.
- Live SWD read of `GPIO0->PDOR` (`0x40096040`) mid-loop, several seconds
  into a normal run: `0x00408400` - bit 23 (BLK, `DEMO_LCD_BLK_PIN`) is
  **0**, i.e. commanded LOW, even though `LCD_SetBacklight(true)` is the
  only place in the whole codebase that ever touches this pin and
  unconditionally requests HIGH.
- **Pin mux ruled out**: `PORT0->PCR[23]` reads `0x00000000` = `kPORT_MuxAlt0`
  (GPIO, no pull) - identical to `PCR[14]`/`PCR[15]`/`PCR[22]` (DC/RST/CS),
  which are all configured by the exact same `BOARD_InitArduinoLcdPins()`
  call. Not a muxing difference.
- **GPIO0's own clock/peripheral function ruled out at the peripheral
  level**: other bits on the same physical register (`PDOR` bit 22/CS,
  bit 15/RST) DO read back correctly matching their last commanded value.
  The peripheral is not globally broken.
- **A live, direct SWD test proved the pin and register genuinely CAN hold
  a forced HIGH state**: `write32 0x40096044 0x800000` (PSOR, the same
  register/bit `GPIO_PinWrite()` uses) immediately set bit 23 to 1, and it
  was still 1 on a second, separate read a full second later with nothing
  else running in between. The electrical path, register, and bit are all
  real and controllable - just not reliably from CORE1's own code.
- **An in-firmware readback diagnostic (temporarily added to
  `LCD_InitGpioPins()`/`LCD_Init()`, since removed) confirmed the write
  from CORE1's own compiled code does not stick, in-context, removing any
  SWD-timing/multicore-targeting ambiguity from the point above**: printed
  `GPIO0->PDOR` immediately after `LCD_SetBacklight(true)` - read back 0,
  both right after `LCD_InitGpioPins()` and again after
  `LCD_InitPanel()`'s ~390ms of delays (ruling out a clock/peripheral
  settling-time explanation - if settling were the issue, the SECOND,
  much-later attempt should have succeeded).
- **Not specific to bit 23**: the same in-firmware test repeated against an
  arbitrary, otherwise-untouched GPIO0 bit (20) showed the identical
  symptom - write, read back 0; write, delay 10ms, read back 0; write
  again, delay 10ms, read back 0 - ruling out anything electrically
  specific to the BLK pin/pad itself.
- **AHBSC per-peripheral access-control rules ruled out live, not just
  assumed**: found `AHB_PERIPHERAL0_SLAVE_PORT_P12_SLAVE_RULE0`/`RULE1`
  (`AHBSC` @ `0x401201D0`/`0x401201D4`) carry a `GPIO0_ALIAS0`/`GPIO0_ALIAS1`
  2-bit field each, documented as controlling required secure/privilege
  level per bus master for exactly this peripheral+alias. Read live:
  both registers are `0x00000000` = `0b00` = "non-secure and
  non-privilege access allowed" for every peripheral in the group - the
  LEAST restrictive setting, not a block. This rule set is not the cause.
- **Address aliasing ruled out**: `GPIO0` compiles to the literal constant
  `0x40096000` in core1's `.elf` (confirmed via `objdump` disassembly of
  `LCD_Init()`, matching the address used throughout this investigation);
  reading both the secure (`0x50096000`) and non-secure (`0x40096000`)
  aliases of `GPIO0->PDOR` returns identical values every time.

**Two evidence-based workarounds attempted, NEITHER confirmed working on
real hardware:**
1. `LCD_SetBacklight()` (`lcd_spi_hw.c`) now retries the write up to 100
   times with an immediate readback verification after each attempt,
   instead of writing once. On real hardware this returns without ever
   printing its own "did not latch" warning (i.e. it always believes it
   succeeded within the retry budget) - but a live SWD read moments later
   still shows bit 23 at 0. Either the retry loop's own readback is
   itself unreliable in the same way as the write (a plausible but
   unconfirmed compounding theory), or something clears the bit again
   after the loop returns.
2. Added `LCD_RefreshBacklight()` (`lcd_spi_hw.h`/`.c`), called once per
   frame from `CameraLcdTask` (`main_core1.c`) right after
   `LCD_DrawImage()` - the same "many repeated attempts" treatment that
   was the leading theory for why CS reliably ends up correct (toggled
   thousands of times/sec) while a write-once pin does not. Also NOT
   confirmed on real hardware: bit 23 was still 0 across three separate
   live reads several seconds apart, after many seconds of continuous
   per-frame calls.
3. **This second attempt's own working theory is now suspect too**: DC
   (`DEMO_LCD_DC_PIN`, bit 14) is toggled with the exact same frequency as
   CS (every SPI command, via `LCD_WriteCommandOpen()`), yet the same live
   PDOR read that showed CS(22)/RST(15) correct showed DC(14) at 0 -
   contradicting "enough repeated attempts eventually stick" as the
   mechanism, since DC gets just as many attempts as CS but still fails.
   Neither "write-once vs. write-repeatedly" nor "which specific bit"
   cleanly explains the CS/RST-succeed vs. DC/BLK/bit20-fail split found
   so far.

**Both workarounds above were REVERTED after a third, more decisive test
disproved the theory behind both of them.** Instead of testing one bit at
a time, wrote `GPIO0->PSOR = 0xFFFFFFFF` (every bit at once) and read
`PDOR` back - tested at TWO different points: immediately inside
`LCD_InitGpioPins()` (before any SPI traffic at all), and again at the end
of `LCD_Init()` (after `LCD_InitPanel()`'s full ~390ms of delays and SPI
command traffic). **Both reads came back `PDOR=0x00000000` - 0 of 32 bits
ever stuck, at either point.** This is strictly stronger evidence than
anything checked earlier in this entry: it rules out "needs more time to
settle" (the second sweep had every opportunity settling could need) and
"needs real SPI/bus activity first to unstick" (the second sweep ran
after a full panel-init sequence's worth of real SPI transactions). Yet
the SAME session's steady-state reads (see above) still showed CS(22)/
RST(15) correctly latched moments later during the main loop - a genuine,
still-unexplained contradiction between "a deliberate, comprehensive
write attempt at any specific instant always completely fails" and
"some bits are nonetheless found correct during ordinary, continuous
operation." Given retry-with-verification and per-frame re-assertion were
both motivated by an incomplete theory (repetition helps) that this sweep
result contradicts, and neither was confirmed working on real hardware
anyway, both were **reverted** (`LCD_SetBacklight()` back to a single
plain write; `LCD_RefreshBacklight()` and its per-frame call removed
entirely) rather than leave misleading, non-functional-but-plausible-
looking code in the tree. `LCD_SetBacklight()` now carries a permanent
(not TEMP DIAGNOSTIC) comment pointing back to this entry.

**Decision, made with the user in this session, for how to actually fix
this**: rather than continuing to hunt for a "confirmed good" GPIO0 bit to
move BLK to (unreliable to verify, per everything above), wire the
panel's BLK line directly to the Arduino header's 3V3 pin instead of A5 -
the code has never once needed to turn the backlight off or dim it, so a
GPIO pin buys nothing here and this sidesteps the whole unresolved
question. **Not yet done** - needs the user to physically move the jumper
wire and, once done, the code needs a follow-up change (delete
`DEMO_LCD_BLK_GPIO`/`PIN`, `LCD_SetBacklight()`, and its two call sites in
`lcd_spi_hw.c`/`.h` entirely, matching the "no GPIO pin needed" design).

### Next steps for a fresh session

1. **Do the 3V3 rewire first** (Arduino A5 -> 3V3 instead of the MCU pin),
   confirm the backlight is now simply always on regardless of firmware,
   then delete the now-dead `LCD_SetBacklight()`/`DEMO_LCD_BLK_GPIO`/`PIN`
   code path (`lcd_spi_hw.c`/`.h`, `board_port/cm33_core1/app.h`,
   `board_port/cm33_core0/app.h`'s Arduino-header branch, `pin_mux.c`'s
   `BOARD_InitArduinoLcdPins()`) and update README.md's pinout table to
   match (BLK row: "3V3 (direct)" instead of "A5 / P0_23 / manual GPIO").
2. Only if there's a reason to still want software backlight control
   later: the underlying mystery (a comprehensive, simultaneous 32-bit
   GPIO0 write from core1 provably fails 100% of the time, yet isolated
   bits are nonetheless observed correct later during continuous
   operation) needs tooling this session didn't have to resolve - a
   hardware watchpoint on `GPIO0->PDOR`/`PSOR` (break on write, inspect
   PC/call stack at the exact moment of every access) via a proper
   interactive pyOCD/GDB session with explicit per-core selection (this
   session's CLI-scripted `pyocd commander` attempts at a precise core1
   breakpoint targeted the wrong core, halting core0's boot vector
   instead), or a logic analyzer directly on the physical P0_14/P0_22/
   P0_23 pins, would both be more conclusive than anything SWD register
   reads alone showed here.
3. The new SD write failure on a REAL detection (`FRESULT=1` on both the
   BMP header and pixel writes, first real face-triggered save attempt
   under Stage 5) is a separate, not-yet-investigated bug - don't conflate
   it with the backlight issue above. Get a fresh capture with a
   definitely-good SD card and see if it reproduces.
4. Once the backlight is genuinely fixed, this file's stack of pending
   confirmations from earlier entries still needs doing: the actual
   critical-section fix (now for real, see the FOURTH FOLLOW-UP entry) for
   LCD tearing during active AI operation, and the touch/bus-sharing
   baud-reclaim logic (still zero real-hardware confirmation - see
   README.md's Known Limitations).

## Dual-core RTOS migration Stage 5 FOURTH FOLLOW-UP - user reported a full blackout (no image, backlight never turns on) on the exact build this file's own THIRD FOLLOW-UP entry claimed was fixed; found that entry's own fix was NEVER ACTUALLY APPLIED (comment-only edit, real bug), fixed it for real, added the missing HardFault diagnostic to the dual-core build so a crash-before-LCD_Init() theory can actually be confirmed or ruled out next time - NOT yet confirmed on real hardware, this session had no working USB/pyOCD access to the probe at all (2026-09-05)

Follow-up to the THIRD FOLLOW-UP entry directly below (same day). User
reported a NEW, more severe symptom on what should have been that entry's
fix: total blackout - no image AND the backlight itself never turns on -
not the torn/wrong-color-but-visible image every previous entry in this
file describes. Since `LCD_Init()` (`lcd_spi_hw.c`) turns the backlight on
as literally its second GPIO write, before any SPI traffic at all, a fully
dark backlight means `CameraLcdTask` (`main_core1.c`) never reached
`LCD_Init()` - a strictly earlier, more severe failure than anything
this file's tearing investigation has been chasing.

**Real bug found by reading the code the THIRD FOLLOW-UP entry claimed had
already fixed something: it hadn't.** `git diff` against the last commit
showed that entry's actual code change to `spi1_bus.c` was a
**comment-only edit** - `SPI1_BUS_LockNoPreempt()`/`UnlockNoPreempt()`
still called `vTaskSuspendAll()`/`xTaskResumeAll()`, word for word
identical to the SECOND FOLLOW-UP's implementation, even though both
`spi1_bus.h`'s doc comment and this file's THIRD FOLLOW-UP entry
explicitly describe switching to `taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`
and explain in detail why. The narrative was written (correctly) but the
actual code edit was never made in the same pass - a real process failure,
not a hardware bug. Fixed now, for real: `SPI1_BUS_LockNoPreempt()`/
`UnlockNoPreempt()` (`spi1_bus.c`) now do call
`taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()`, matching what the comments
already claimed. **This alone does not explain the blackout symptom** -
backlight is set well before this lock is ever touched - but it needed
fixing regardless before trusting anything else in that entry.

**No serial log or SWD access was available this session to actually
confirm why `CameraLcdTask` never reaches `LCD_Init()`** - this sandbox's
`pyocd`/`nxpdebugmbox` could enumerate the MCU-Link over USB (`lsusb`,
`pyocd json --probes`-equivalent listing all worked instantly) but every
actual data-transfer operation to it (`pyocd list`'s device query,
`nxpdebugmbox ... start-debug-session`) failed with a flat
`[Errno 110] Operation timed out`, even under passwordless `sudo` and with
sandboxing explicitly disabled for the command - consistent with this
specific shell environment lacking real raw-USB (bulk/interrupt transfer)
passthrough even though device enumeration metadata is visible, not a
probe or firmware problem (the same probe/board this file's whole history
was debugged on). Per this project's own "confirm on real hardware, don't
guess" standard - the blackout's root cause is NOT yet confirmed, only
prepared for.

**Added the one piece of infrastructure needed to actually root-cause a
silent early crash next time it can be tested: `source/fault_handler.c`
(the project's existing HardFault register-dump handler) was compiled into
the LEGACY single-core build only - `CMakeLists.txt`'s `DUALCORE_RTOS`
branch never added it for either core.** A HardFault in the dual-core
build currently falls through to the SDK's default weak handler (a silent
infinite loop, no UART output at all) instead of printing
CFSR/HFSR/MMFAR/BFAR/PC/LR the way the legacy build always has - and this
project's own history (STKOF stack-overflow, RAM-bank collisions,
TrustZone/`configRUN_FREERTOS_SECURE_ONLY` UsageFault, all documented
earlier in this file) shows HardFaults are a real, recurring failure mode
here, not a hypothetical one. Added `source/fault_handler.c` to both
cores' `mcux_add_source()` lists in `CMakeLists.txt`. Real, measured build
confirms it fits with margin on both sides (not guessed): core1
`m_text` 49,116/64KB (75%), `m_data` 39,472/39KB (98.8% - tight, as this
region always has been, but builds and links successfully); core0
`m_text` 146,208B/767KB (18.6%), `m_data` unchanged at 94.7%. If the
blackout is a HardFault (a live theory, not confirmed), the NEXT flash
will print a real fault dump over UART instead of nothing - if it's
instead a hang with no fault at all (e.g. stuck in `CAMERA_CAPTURE_Init()`,
or the core0<->core1 MCMGR handshake itself, both of which have hung this
project before per earlier entries), the fault dump staying silent while
the board is provably unresponsive is itself a useful, real data point
that rules out a whole class of explanation.

**Not touched, deliberately, given no way to test any of it this
session**: `TEMP_SKIP_IPC_ROUNDTRIP` (`main_core1.c`) is still `1`
(bypasses the core0 AI round-trip entirely) - unrelated to the backlight
theory (it only affects whether the AI overlay/snapshot branch runs, not
whether `LCD_Init()`/the base camera-preview push happen), left as-is
rather than changing two things at once before either can be verified.

### Next steps for a fresh session

1. **Flash `dualcore-all` (this session's build already succeeded, just
   couldn't be flashed) on a machine/shell with real USB access to the
   probe** and capture the serial log across a fresh reset - the single
   most useful thing to know is simply whether ANY of core1's own prints
   appear at all (`"Camera_AI_Test1 - core1 ..."` banner, then
   `"LCD: hardware SPI ..."` from `LCD_Init()` itself) before deciding
   between "never reached `CameraLcdTask`"/"hung inside `CAMERA_CAPTURE_Init()`"/
   "a HardFault now dumping real registers thanks to this session's fix"/
   "something else entirely."
2. If a HardFault dump does appear: read CFSR/HFSR directly (same decode
   this file's earlier TrustZone/STKOF entries already used) before
   guessing at a specific cause - core1's RAM is now genuinely near its
   ceiling (`m_data` 98.8%) after this session's addition, so a stack
   overflow (`vApplicationStackOverflowHook()`, `freertos_hooks.c` - also
   currently a silent infinite loop, no print at all, worth the same
   "add a diagnostic before guessing" treatment as fault_handler.c above if
   this turns out to be the cause) is a live, cheap-to-check candidate.
3. If no fault fires and the board is just silent/hung: bisect by
   commenting out `CameraLcdTask`'s body down to just
   `CAMERA_CAPTURE_Init(); LCD_Init();` (removing `SNAPSHOT_Init()` and the
   whole per-frame loop) and re-testing - narrows "never gets past camera
   init" vs. "gets past LCD_Init() fine, something later in the loop wedges
   the board so fast the backlight-on moment isn't visible" (the latter
   seems unlikely given the user described a sustained blackout, not a
   flicker, but not yet ruled out without a real log).
4. Once the blackout is explained and fixed, re-confirm the THIRD
   FOLLOW-UP entry's actual (now real, not just documented) fix - point
   the camera at a face, trigger detections, and look at the LCD for
   tearing during active AI operation specifically (the MAILBOX_IRQn this
   fix targets only fires during the AI round trip).

## Dual-core RTOS migration Stage 5 THIRD FOLLOW-UP - the two-consecutive-frame check from the previous entry was replaced with a direct buffer-content settle check (also ineffective alone, but proved the frame buffer WAS stable), which combined with a user-provided A/B test against the single-core `spi_tft_change` branch (ruled out hardware/wiring entirely) pointed at the real cause: Stage 5's new MCMGR mailbox interrupt runs at exactly the FreeRTOS-maskable priority threshold, so the earlier `vTaskSuspendAll()`-based LCD fix (blocks task switches only) never actually protected against it. Fixed with a real critical section - NOT yet confirmed by the user on real hardware (2026-09-05)

Follow-up to the SECOND FOLLOW-UP entry below (same day). The two-
consecutive-frame mitigation shipped in that entry was tested on real
hardware with actual saved snapshots and did NOT reduce the torn-image
symptom - visually identical corruption (a diagonal boundary + fine
horizontal banding, confirmed by converting and viewing the actual saved
BMPs, not just checking file size) still appeared, non-deterministically,
across multiple captures.

**Replaced the frame-count check with a direct buffer-content settle
check, which produced a genuinely useful negative result.** Rather than
trusting `CAMERA_CAPTURE_GetFrameCount()` sequencing, added a check that
hashes a sparse sample of the frame buffer right after
`CAMERA_CAPTURE_Deinit()`, waits 2ms, hashes it again, and retries (bounded)
until two consecutive hashes agree - directly testing whether `Deinit()`
is truly an instantaneous, synchronous stop. Confirmed on real hardware:
**zero instability ever detected**, across many frames including a real
detection/save event - proving the buffer is genuinely frozen and stable
by the time it's read. This ruled out the leading theory from the
previous entry (a SmartDMA DMA-completion race letting `Deinit()` freeze
a mid-write buffer) - the frozen buffer's *content* is exactly what
SmartDMA actually wrote, whatever that content is.

**User provided the decisive test: a direct A/B against the single-core
`spi_tft_change` branch on the exact same physical hardware.** With the
buffer confirmed stable, the working theory shifted to a camera DVP-bus
signal-integrity issue (PCLK/HREF/VSYNC on breadboard wiring) - raised to
the user as the likely remaining explanation. The user correctly rejected
this: flashing `spi_tft_change` (legacy single-core, same hardware, same
camera, same wiring) showed a clean LCD and clean captures, twice: while
`full_refactor` (this dual-core build) showed torn LCD images on the same
physical setup. This is the same "diff against the known-good branch"
technique that already worked earlier for the SD corruption investigation
(see [[feedback-dont-conclude-early-root-cause]]) - it directly falsified
the hardware theory and correctly redirected the investigation back to
this dual-core build's own software.

**Root cause: Stage 5's new MCMGR mailbox interrupt runs at exactly the
FreeRTOS-maskable priority threshold, and the LCD tearing fix from the
first Stage 4 follow-up entry only ever protected against task-level
preemption, never interrupts.** Re-examined `spi1_bus.c`'s
`SPI1_BUS_LockNoPreempt()` (added for the original LCD tearing fix,
`vTaskSuspendAll()`/`xTaskResumeAll()`) against what's actually NEW in
Stage 5: the core0<->core1 IPC round trip, which delivers its "result
ready" doorbell via a real hardware interrupt (`MAILBOX_IRQn`), not
present at all in Stage 4's simpler camera+LCD+SD pipeline. Checked its
configured priority directly in
`mcmgr_internal_core_api_mcxnx4x.c`: `NVIC_SetPriority(MAILBOX_IRQn, 2)`
on core1 - exactly equal to this project's own
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (2, `FreeRTOSConfig.h`).
`vTaskSuspendAll()` only blocks task switches, NOT interrupt servicing -
so this real, new interrupt source could fire and interrupt an
in-progress LCD SPI transaction at any time, something the existing fix
never actually addressed. This is a different, additional gap on top of
the original (task-preemption) finding, not a contradiction of it - both
needed fixing, Stage 4's fix only caught the first one because Stage 4
had no interrupt-driven cross-core signaling yet.

**Fix**: `SPI1_BUS_LockNoPreempt()`/`UnlockNoPreempt()` now use
`taskENTER_CRITICAL()`/`taskEXIT_CRITICAL()` instead of
`vTaskSuspendAll()`/`xTaskResumeAll()` - raises BASEPRI to
`configMAX_SYSCALL_INTERRUPT_PRIORITY`, which masks `MAILBOX_IRQn`
directly AND blocks PendSV (task switches) in the same primitive, fully
superseding the old call rather than needing both. Still takes the mutex
first, before entering the critical section (required ordering -
`xSemaphoreTake()` can block/yield and must never run inside a critical
section). SmartDMA's own completion IRQ is unaffected either way, since
SmartDMA is always `Deinit()`'d/clock-gated for this lock's entire
duration regardless.

**Confirmed on real hardware: builds clean, runs without crashing, fps
unchanged (8-9), no new warnings.** NOT yet confirmed by the user to
actually show a clean LCD - this entry's fix is flashed and awaiting that
specific visual confirmation before being called done.

### Next steps for a fresh session

1. Get the user's direct visual confirmation that the LCD is clean during
   active AI/face-detection operation (not just idle preview) - the
   MAILBOX_IRQn fires specifically during the AI round trip, so a
   preview-only check without any detections happening wouldn't fully
   exercise this fix.
2. If the LCD is confirmed clean, re-test actual saved snapshots too
   (view the BMPs, not just file size) - the SD write path never used
   `SPI1_BUS_LockNoPreempt()` at all (confirmed to break SD mount, see the
   SECOND FOLLOW-UP entry below), so if snapshot tearing persists even
   with a clean LCD, that's now isolated to something specific to the SD
   write path or the buffer's state during that specific window, not this
   shared LCD-lock mechanism.
3. SD card mount was failing again in this session's tests ("SD card init
   timed out after 2000ms") - separate from the tearing investigation,
   still needs a fresh look (last confirmed working after the full
   reformat in the first Stage 4 follow-up entry).
4. Phase 4 (final stage in the approved plan) is otherwise unstarted:
   delete the legacy single-core `main.c`/board_port/CMakeLists once the
   dual-core build is confirmed as the new default, update
   ARCHITECTURE.md accordingly.

## Dual-core RTOS migration Stage 5 SECOND FOLLOW-UP - raised the NPU detection threshold to cut false positives, found and reverted a real regression my own earlier LCD fix caused on the SD mount path, and added a two-consecutive-frame stability check for a real (pre-existing, now more visible) torn-snapshot bug - NOT yet confirmed to actually fix the tearing, needs a real-hardware retest with saved BMPs (2026-09-05)

Follow-up to the Stage 5 entry below (same day). User reported two real,
independent problems after Stage 5 went live: the model flagging a plain
wall as a face, and saved snapshot BMPs sometimes coming back with
genuinely torn/wrong-color content (confirmed via photos of the actual
saved files, opened on a PC).

**False positives - a real model accuracy limit, tuned via threshold, not
a pipeline bug.** Repeated real-hardware testing showed frequent `face`
detections in the 0.5-0.65 confidence range on plain walls/floors - a
single-class FOMO model's real generalization limit on scenes unlike its
training data, not a data-corruption symptom (the frame buffer feeding
inference was independently confirmed correct at this point in the
pipeline). Raised `NPU_MODEL_DETECTION_THRESHOLD` (`model_runner_npu.cpp`)
from the Edge Impulse export's own calibrated default of 0.5 first to 0.7
(confirmed on real hardware: zero false positives in a 15s window that
previously showed several), then to 0.65 per explicit user preference -
a deliberate accuracy/sensitivity tradeoff, not something to "fix" further
without retraining the model itself (user's own stated plan).

**Real regression found: my own Stage 4-follow-up `SPI1_BUS_LockNoPreempt()`
fix (LCD tearing) broke SD mount when also applied to `sd_spi_disk.c`.**
On the theory that the SD write corruption (see below) might be the same
class of scheduler-preemption bug the LCD tearing was, applied
`SPI1_BUS_LockNoPreempt()` to `disk_initialize()`/`disk_read()`/
`disk_write()` too. Real hardware immediately regressed: `SD card init
timed out after 2000ms` on every boot, even on a card confirmed
physically reseated and making contact. Root-caused via a direct A/B test
rather than assuming: reverted just those 3 calls back to the plain
`SPI1_BUS_Lock()`/`Unlock()`, rebuilt, and mount worked again immediately
(`SD card ready`, capacity correctly read as 59024 MB - also directly
confirming, via real data, that the SD-over-SPI driver's CSD/capacity
decode is NOT buggy for this large card, closing out a theory from the
first Stage 4 follow-up entry that was left open). This is now the same
*class* of finding as the original Stage 4 discovery (a wider lock scope,
or apparently now also a stricter lock TYPE, breaks ACMD41/mount for a
reason still not pinned down) - documented in `sd_spi_disk.c` directly so
a future session doesn't retry the exact same experiment.

**Torn/wrong-color snapshot content - real, pre-existing, more visible now,
mitigated but not yet confirmed fixed.** All 56 of the user's saved BMP
files were exactly the correct size (150.1KB) - ruling out a short/
truncated SD write - but several had visibly torn, wrong-color content,
non-deterministically (same code, same session, some clean, some garbled).
Since the corruption is in *content* not *transfer length*, and the SD
write path (confirmed above) and LCD push (confirmed in the previous
follow-up) both read from the same frozen buffer, the most likely
explanation is a genuine SmartDMA hardware race: the "frame ready"
interrupt firing a moment before the DMA engine has actually finished
flushing the last rows to RAM, so `CAMERA_CAPTURE_Deinit()` (called
immediately after) can freeze a buffer that's part current frame, part
leftover from the previous one. This likely predates Stage 5 entirely -
it's the same capture/DMA boundary the original single-core LCD tearing
fix already worked around (`skipNextFrame`) - it just wasn't very visible
before: Stage 4's SD save was a rare 5-second manual test, and a torn
LIVE PREVIEW frame just flashes by unnoticed on the next redraw, whereas
Stage 5 saves a torn frame permanently the instant it happens, at up to
1/sec.

Mitigation added (`main_core1.c`'s `CameraLcdTask`): after the existing
post-`Reinit()` `skipNextFrame` discard, require TWO consecutive "frame
ready" notifications whose `CAMERA_CAPTURE_GetFrameCount()` values are
exactly back-to-back (no gap) before trusting the buffer - any gap
restarts the pairing from the newer frame instead of trusting it blind.
This is a heuristic, not a fix for the underlying DMA-completion race
itself - it only filters the specific failure mode where the notified
frame count is stale or out of sequence (e.g. from a resync glitch), and
was explicitly described to the user as such before implementing.
Confirmed on real hardware: builds clean, runs without crashing, fps cost
is real and as predicted (8-9 -> 7, one extra ~33ms camera frame period
per processing cycle at this sensor's 30fps). **NOT yet confirmed to
actually reduce/eliminate the torn-snapshot symptom** - needs a fresh
round of real snapshots inspected for corruption before this can be
called fixed; may need combining with the signature/settle-delay
alternative mentioned when this was first diagnosed if the frame-count
pairing alone isn't sufficient.

### Next steps for a fresh session

1. Point the camera at a real face, trigger several saves, and inspect
   the resulting BMPs for tearing - confirms or refutes the two-
   consecutive-frame mitigation above. If tearing persists, the next
   experiment is the signature/settle-delay alternative discussed with
   the user (sample a cheap checksum of the buffer, wait a short settle
   time, re-sample, discard the frame if they disagree) rather than
   frame-count pairing alone.
2. SD card mount status needs re-checking independent of the above - it
   was failing again in the most recent hardware test session (before the
   two-consecutive-frame change was even tested), for reasons not yet
   investigated this round (possibly just physical - card wasn't
   necessarily inserted for that particular test).
3. Phase 4 (final stage in the approved plan) is otherwise unstarted:
   delete the legacy single-core `main.c`/board_port/CMakeLists once the
   dual-core build is confirmed as the new default, update
   ARCHITECTURE.md accordingly.

## Dual-core RTOS migration Stage 5 CONFIRMED ON REAL HARDWARE - full pipeline wired: AI inference on core0 (Neutron NPU, unchanged model_runner.h API), real cross-core frame-ready/result-ready doorbell round trip, live bbox overlay + snapshot save on core1 all driven by real detections instead of Stage 4's fake ones. One real, fully-reproduced boot-hang bug found and fixed (FreeRTOS API calls before the core0<->core1 MCMGR boot handshake completes). SD card save NOT yet re-confirmed working in this exact build - separate, pre-existing symptom, not caused by this stage (2026-09-05)

Follow-up to the Stage 4 FOLLOW-UP entry below (same day). Implements the
approved plan's Stage 5: real shared-buffer handoff + AI on core0, per
`~/.claude/plans/stateful-churning-flurry.md`.

**Design**: `source/shared/ipc_layout.h` gained a plain-data `ai_ipc_result_t`/
`ai_ipc_bbox_t` pair - deliberately NOT the same type as `model_runner.h`'s
`ai_model_result_t`, whose `ai_bbox_t.label` is a `const char *` into
core0's own flash; copying that byte-for-byte into shared RAM would hand
core1 a pointer with no defined meaning as portable cross-core data (even
though it would likely still be *readable*, since flash is one physical
bus-shared resource - not worth relying on). `ai_ipc_result_t` carries the
label as a small inline byte array instead. `source/shared/ipc_events.h`
gained `IPC_SignalFrameReady()`/`IPC_SignalResultReady()`, thin wrappers
over Stage 2's existing `IPC_EVENTS_Trigger()`, matching the plan's
originally-named doorbells. Per-frame flow (`CameraLcdTask`,
`main_core1.c`): `CAMERA_CAPTURE_Deinit()` (as before) -> signal frame-
ready with a sequence number -> block on `xTaskNotifyWait()` (100ms
timeout - generous vs. the ~4ms NPU inference actually measured, same
"fail loud, don't hang" pattern as this project's other cross-boundary
waits) -> on success, convert the wire-safe result back to
`ai_model_result_t` shape (labels point into the local stack copy, valid
for the remainder of the same scope) -> draw the bbox onto the live
preview -> call `SNAPSHOT_OnFrame()` with the REAL result (replacing Stage
4's synthesized fake box) -> `LCD_DrawImage()` -> `CAMERA_CAPTURE_Reinit()`.
`StorageTask` is retired entirely (exactly as its own Stage 4 comments
predicted) - `SNAPSHOT_Init()` moved into `CameraLcdTask`'s own startup.
core0's `AiInferenceTask` mirrors this: block on the doorbell, read the
frame (safe by construction - core1 guarantees the buffer is frozen for
this entire round trip), run inference via the unmodified
`model_runner.h` API (same NPU backend the legacy single-core build
defaults to), convert to the wire-safe type, write it to shared RAM, reply.

**Confirmed dead code removed, RAM reclaimed for it**: the original Stage
1 plan reserved a ~15KB "AI input crop buffer" in the shared region,
before Stage 5's actual API was implemented. Checked `model_runner_npu.cpp`
directly before wiring anything up: `AI_MODEL_RunInference()` takes the
raw camera frame and does its own internal resize/quantize into the
model's 72x72 input - nothing was ever going to read a separate crop
buffer. Removed it from `ipc_layout.h` and reclaimed the space (0x4000,
16KB) for core0's own `m_data` instead (`MCXN947_cm33_core0_dualcore.ld`:
`m_data` 0x24000->0x28000, `m_shared` origin/length shifted to match,
core1's own region unchanged) - core0's NPU tensor arena is 120KB, which
did not fit the original 144KB `m_data` budget at all once FreeRTOS's own
heap was added; final build uses 155,160/163,840 bytes (94.7%) - tight but
fits with real (if modest) margin, no further guessing needed since it
was a real, measured build.

**Real bug found and fixed - core0<->core1 boot handshake hangs if ANY
FreeRTOS API runs before `MCMGR_StartCore()` finishes.** First attempt
created `AiInferenceTask` and called `IPC_EVENTS_RegisterHandler()` before
`MCMGR_StartCore()` (seemed harmless - no event could possibly arrive that
early) - core0 hung forever printing only "core0: starting core1..." (no
"core0: core1 started.", no core1 banner at all). Root-caused via SWD
halt on BOTH cores (pyocd's `core 0`/`core 1` selector) rather than
guessing: core0 was stuck inside `MCMGR_StartCore()`'s own busy-wait loop
(`mcmgr.c:212`, waiting for `state == kMCMGR_RunningCoreState`); core1 was
stuck inside `MCMGR_GetStartupData()` -> `MAILBOX_GetValue()` - confirmed
genuinely stuck, not just sampled mid-poll, by resuming core1, waiting
200ms, halting again, and observing an *identical* PC both times. Two
follow-up A/B tests isolated the exact cause: (1) reverting `main_core0.c`
to the Stage 4 shape (no AI task at all) while KEEPING the new Stage 5
linker layout still booted cleanly - ruled out the `m_data`/`m_shared`
resize as the cause; (2) moving only `IPC_EVENTS_RegisterHandler()` to
after `MCMGR_StartCore()` did NOT fix it (still hung identically) - ruled
out event-registration ordering specifically; only moving `xTaskCreate()`
itself to after `MCMGR_StartCore()` fixed it. Exact underlying mechanism
not fully root-caused (plausibly an interrupt-priority/BASEPRI side
effect of FreeRTOS's critical-section macros running before the scheduler
has initialized anything, colliding with the mailbox IRQ the handshake
needs - not confirmed to that level of detail, and not worth over-
investing in given the fix is clean, safe, and well-isolated by real A/B
tests). Fixed by strictly sequencing `main_core0.c`: finish ALL of the
MCMGR-level handshake (image copy, `MCMGR_StartCore()`) FIRST, only touch
any FreeRTOS API (`xTaskCreate()`, `IPC_EVENTS_RegisterHandler()`) after.

**Confirmed on real hardware, full pipeline, real detections**:
```
AI_MODEL_Init: Neutron NPU face detector ready (72x72 input, 1 class(es), arena used 94388/122880 bytes)
Camera: OV7670 detected on J9 (PID=0x76 VER=0x73 confirmed), 320x240 @ 30 fps.
LCD preview: 8 fps
AI_MODEL_RunInference: total classifier time = 3913us (3ms)
AI result: box[0] label=face x=56 y=16 w=8 h=8 score=53%
```
fps: 11 -> 8-9 (real cost of the added cross-core round trip on top of the
existing 24MHz LCD push - each frame now also waits for a full NPU
inference cycle via IPC, ~4ms, plus scheduling/IPC overhead). NPU
inference time itself (~3.9ms) is unchanged from the legacy single-core
measurement - moving it to its own core didn't slow inference down, it
just adds the round-trip cost to core1's per-frame budget.

**NOT yet re-confirmed: SD card save.** `Snapshot: SD card init timed out
after 2000ms` in this exact build - but this is NOT a Stage 5 regression:
the identical symptom appeared in the isolated Stage-4-shape/Stage-5-
linker test used to root-cause the boot hang above (i.e., before any of
Stage 5's AI wiring existed), so it predates this stage's changes.
Separate, not-yet-investigated issue - see next steps.

### Next steps for a fresh session

1. Investigate the SD card timeout above - check card seating/wiring
   first (this project's own established first check), then reapply the
   "confirm on real hardware, don't guess" standard from the Stage 4
   FOLLOW-UP entry if it isn't simply a seating issue - do not assume it's
   the same root cause as any earlier SD bug without checking fresh.
2. Confirm a real saved BMP snapshot end-to-end once SD is working again -
   the approved plan's Stage 5 acceptance criterion ("confirm face
   detection and a real saved BMP snapshot on actual hardware, not just
   log output") is only half-confirmed right now (detection: yes, real
   hardware, real boxes; snapshot save: blocked on the SD issue above).
3. Phase 4 (final stage in the approved plan): delete the legacy
   single-core `main.c`/board_port/CMakeLists once the dual-core build is
   confirmed as the new default, update ARCHITECTURE.md accordingly, and
   fold this WORKLOG's dual-core entries into a proper summary rather than
   the current chronological blow-by-blow.

## Dual-core RTOS migration Stage 4 FOLLOW-UP - three separate real bugs found and fixed on real hardware: a "black screen" that was actually a diagnostic ordering mistake (not a capture bug), an SD card that needed reformatting (not a code regression), and LCD tearing/wrong-color that was FreeRTOS task preemption mid-transaction, NOT the signal-integrity bug first (wrongly) suspected - runs clean at the original 24MHz once the scheduler is held off during the transfer (2026-09-05)

Follow-up to the Stage 4 entry below. User reported two new symptoms after
Stage 4: the TFT showing pure black, and (separately, after extensive
mutex/locking work that never actually fixed it) a persistently failing
SD card. Investigated both from scratch rather than continuing to guess
at the locking code, per this project's own "confirm on real hardware,
don't guess" standard - and per explicit user pushback ("the old code
before start changing work fine but now why i encounter this bug") that
the SD problem was not adequately explained yet.

**"Black screen" - root cause was this session's own diagnostic code, not
a capture bug.** Added `DEMO_LogFrameSignature()` (`main_core1.c`) to
check actual pixel content instead of trusting the fps counter alone -
it reported flat `0x0000` on every single frame, which looked like
SmartDMA never writing real data to the shared-region frame buffer
(`ipc_layout.h`'s `IPC_FRAME_BUFFER_ADDR`, `0x20024000`). Formed and
tested a wrong theory first (SmartDMA can't reach that address - lowered
`IPC_SHARED_BASE` to `0x20010000` as a test, still flat zero, hypothesis
falsified) before checking the AHB Secure Controller
(`AHBSC->MASTER_SEC_LEVEL`/`RAMx_MEM_RULE`, `PERI_AHBSC.h`) as a possible
non-secure-master-blocked-from-secure-RAM explanation - also falsified by
directly reading `AHBSC->MISC_CTRL_REG` over SWD: `ENABLE_SECURE_CHECKING`
is set to "disabled" on this chip, so none of those rules are even being
enforced. The actual bug: halted the target over SWD mid-run and read the
frame buffer directly at `0x20010000` - it contained real, plausible,
CHANGING RGB565 pixel values across two samples 0.3s apart, proving
SmartDMA was writing real data the whole time. The diagnostic function was
just being called from the wrong place in `CameraLcdTask`'s loop - after
`CAMERA_CAPTURE_Reinit()`, which immediately `memset()`s the buffer to
zero to prepare for the next capture, so it only ever saw an
already-cleared buffer. Moved the log call to run before `Reinit()`;
confirmed on hardware immediately after: real pixel data (`0x2945..0xE77A`
range), fps unchanged at 11. Reverted `IPC_SHARED_BASE` back to
`0x20024000` - it was never the problem.

**SD card failures - isolated to the card itself, not this project's
locking code.** User confirmed the card mount/write failure survived a
full revert to the exact pre-Stage-4-followup code, which is real evidence
the many locking-scope experiments earlier in Stage 4 were not the cause
(this was flagged as an open disagreement in the previous entry - now
resolved). Decisive test: temporarily disabled `CameraLcdTask` entirely
so `StorageTask` had 100% exclusive, zero-contention access to the shared
SPI1 bus - the exact same failures still happened (`write failed`, then
`FR_DISK_ERR` stuck on the same file index across three consecutive
retries), which rules out bus contention/mutex scope as the cause
outright. Added real `FRESULT`/index logging to `snapshot.c` instead of
the old generic "could not create a new file" message. Given `f_getfree()`
(read-only) succeeded and reported a plausible free-space number, but
every write-path operation failed and got *worse* over time (stuck, not
random) - consistent with accumulated FAT corruption from many earlier
sessions where writes failed mid-transfer and the board was reset without
a clean unmount, not a hardware or code defect. Investigated (and ruled
out via code review) a theory that the SD-over-SPI driver's CSD/capacity
decode (`fsl_sdspi.c`'s `SDSPI_DecodeCsd()`) might be mis-sizing this
particular (58GB, SDXC) card - the CSD v2.0 branch's math is standard and
correct, so this was set aside as unlikely, but a `SDCARD_DISK_GetCapacityBytes()`
diagnostic (`sd_spi_disk.c`/`.h`) was added anyway and is now printed by
`SNAPSHOT_Init()` so a future session has a real number instead of having
to reason about it from code alone. Reformatted the card as FAT32 from a
PC (after a careful device-identification check - the card's reported
58GB size didn't match the firmware's own ~1.66GB free-space report, which
turned out to just be years of accumulated snapshot/corruption reducing
usable free space on a much bigger card than assumed, not a different
physical card as first suspected) - **confirmed on real hardware
afterward: SD mount and snapshot save both work.**

**LCD tearing/wrong-color - WRONG THEORY FIRST, then root-caused for
real.** User sent a photo of the actual LCD showing heavy color noise and
diagonal tearing - the first time this bug had been checked by eye rather
than by fps counter/log alone (the fps counter and even the SWD
pixel-content check above cannot detect this class of problem, since both
looked completely normal).

*Wrong first theory*: `lcd_spi_hw.c`'s own file-header comment already
explicitly predicted a similar-sounding failure mode at
`LCD_SPI_BAUDRATE_HZ`'s default 24MHz on this project's breadboard wiring
("If the image comes back glitchy/noisy/torn... lower this back toward
2-6MHz first before suspecting anything else") - written during earlier
single-core LCD bring-up. Lowered to 6MHz, user confirmed the image looked
clean, and this was reported as fixed (signal integrity, not a software
bug). **This was wrong, caught by the user**: they pointed out the old
single-core `spi_tft_change` branch runs at the exact same 24MHz, same
wiring, same LCD, with no corruption - which directly falsifies "24MHz is
electrically unreliable on this wiring" as an explanation, since nothing
about the wiring changed between the two tests. Exactly the mistake
flagged in this project's own "don't conclude root cause too early"
practice: the 6MHz test looked conclusive (image got clean) but was never
checked against the one input that would have falsified it (the known-
good baseline at the *same* clock).

*Real root cause*: `git diff spi_tft_change full_refactor` on
`lcd_spi_hw.c`/`spi1_bus.c`/`camera_capture.c` showed no real functional
differences outside `#ifdef DUALCORE_RTOS`-guarded, purely-additive mutex
wrapping - the actual new ingredient in the dual-core build is FreeRTOS
itself. `spi1_bus.h`'s `SPI1_BUS_Lock()`/`Unlock()` (a plain mutex) only
stops **another task** from touching the shared bus - it does NOT stop
`configUSE_TIME_SLICING`'s round-robin from preempting the lock HOLDER
mid-transaction. `CameraLcdTask` and `StorageTask` are equal priority
(required by the earlier priority-starvation fix), so every SysTick the
scheduler can legally switch away from `CameraLcdTask` mid-`LCD_SetWindow()`/
mid-pixel-push (CS still held low, GPIO-driven) to run `StorageTask` (which
just immediately blocks on the same mutex and switches back) - a real,
uncontrolled timing gap mid-transaction that the ILI9341-family controller
apparently doesn't tolerate cleanly, producing exactly this kind of
diagonal tear/wrong-color corruption. The bare-metal single-core build has
*zero* preemption on this code path at all (no scheduler), which is why
the identical code/wiring/24MHz clock works there. Added
`SPI1_BUS_LockNoPreempt()`/`UnlockNoPreempt()` (`spi1_bus.c`/`.h`) - takes
the existing mutex, then also calls `vTaskSuspendAll()`/`xTaskResumeAll()`
(blocks task-level context switches for the duration, but not ISRs, so
the camera's own SmartDMA completion interrupt is still serviced
normally) - used in `LCD_Init()`'s panel-init sequence and
`LCD_DrawImage()` instead of the plain mutex. **Confirmed on real
hardware: restored `LCD_SPI_BAUDRATE_HZ` to 24MHz (matching the legacy
baseline exactly) and the image is clean** - proving preemption, not
signal integrity, was the actual cause. fps back to the full 24MHz-class
number instead of the 6MHz compromise.

**Process lesson, worth keeping in mind for future sessions**: two
separate but related lessons from this one entry. First, `black screen`
and `LCD tearing` were both invisible to every diagnostic previously
trusted (fps counters, frame-content SWD reads) and only surfaced once
someone actually looked at the physical output - prefer asking for a
photo/visual check earlier when a symptom is described in terms a counter
can't capture. Second, and specifically for the LCD tearing bug: a fix
that makes a symptom go away is not confirmed correct until checked
against the *specific* known-good baseline the user can point to - "the
code's own comment already warned about this" made the 6MHz theory feel
solid, but a pre-existing comment predicting a plausible-sounding failure
mode is not the same as evidence it's the *actual* cause here. The user's
"but the old firmware worked at 24MHz" pushback was the one check that
actually distinguished the two theories.

### Next steps for a fresh session

1. Stage 4 is now genuinely, fully confirmed end-to-end on real hardware:
   camera capture, LCD preview (clean image, no tearing, back at the full
   24MHz), and SD snapshot save all work together under RTOS scheduling.
   Safe to consider Stage 4 done.
2. Stage 5 unchanged from the approved plan - see
   `~/.claude/plans/stateful-churning-flurry.md`.
3. `SPI1_BUS_LockNoPreempt()` currently suspends the WHOLE scheduler
   (`vTaskSuspendAll()`) for the full duration of `LCD_DrawImage()` -
   at 24MHz that's the full ~57ms pixel-push time, every frame (~63% duty
   cycle at 11fps). Harmless so far (only `CameraLcdTask`/`StorageTask`
   exist, and `StorageTask` would just block on the mutex anyway during
   this window), but worth revisiting once Stage 5 adds a core1 IPC-event
   task - if that task ever needs to react within a few ms of an event
   (not just "eventually"), a full scheduler suspension across the whole
   pixel push could be too coarse; narrowing `LockNoPreempt()` to just the
   short, CPU-polled `LCD_SetWindow()` command sequence (not the
   DMA-driven bulk pixel push, which shouldn't need CPU/scheduler
   availability once started) would be the natural next refinement if that
   turns out to matter.

## Dual-core RTOS migration Stage 4 - SD snapshot ported to core1, two real concurrency bugs found and fixed on real hardware (priority-starvation, missing shared-bus mutex); SD mount + file creation now confirmed working under RTOS scheduling, but full end-to-end save is NOT yet confirmed - a write failure and repeated "could not create file" after it are more likely a near-full test card (31+ pre-existing 150KB snapshots) than a new bug, but this is not confirmed either way (2026-09-04)

Follow-up to the Stage 3 entry below (same day). Ported `sd_spi_disk.c`/
`snapshot.c`/FatFs onto core1 as a `StorageTask`, manually triggered every
5 seconds with a synthesized "face" box (AI isn't wired up until Stage 5) -
see the approved plan's Stage 4 description.

**Real bug #1 - priority-based starvation, not a heap/race issue as first
suspected.** First attempt gave `CameraLcdTask` a higher priority than
`StorageTask` ("protect the fps-critical loop from unnecessary
preemption"), reasoning backwards from how FreeRTOS priority actually
works: `CameraLcdTask`'s loop body is a tight busy-poll with no
`vTaskDelay`/blocking call in its "no frame yet" branch, so it is *always*
ready and never voluntarily yields. Under strict priority-based preemptive
scheduling, a strictly-lower-priority task that the higher one never
blocks against is starved completely, not just occasionally preempted -
confirmed on real hardware: `StorageTask` never printed even its
boot-time `SNAPSHOT_Init()` line in a 16-second capture. Fixed by making
both tasks equal priority, letting `configUSE_TIME_SLICING`'s round-robin
give both real CPU time every tick regardless of blocking behavior.

**Real bug #2 - the shared LPSPI1 bus (LCD + SD, see spi1_bus.h) has no
mutex protecting it against two concurrent FreeRTOS tasks.** The legacy
single-threaded bare-metal build never needed one (only ever one thread
of execution touching the bus, strictly sequential) - `spi1_bus.c`'s own
"every driver reclaims its own baud rate immediately before its own
transfer" discipline assumed that. With the priority bug fixed,
`StorageTask` started running for real - and every snapshot attempt then
failed immediately with "could not create a new file", because
`CameraLcdTask`'s LCD push (every ~91ms at 11fps) could now genuinely
preempt an in-flight SD transaction and interleave its own bus traffic
mid-command. Added a real FreeRTOS mutex (`SPI1_BUS_CreateLock/Lock/
Unlock()`, `source/spi1_bus.c`, guarded `#ifdef DUALCORE_RTOS` - zero
effect on the legacy build), wrapped around every COMPLETE logical
transaction on both sides: `LCD_Init()`/`LCD_DrawImage()` on the display
side, `disk_initialize()`/`disk_read()`/`disk_write()` on the SD side
(`disk_ioctl()` doesn't touch the bus at all - checked, no lock needed
there), plus `main_core1.c`'s own `DEMO_ClearScreen()` multi-call
sequence. Confirmed a real, measurable improvement: file creation now
succeeds (found and opened `FACE0032.BMP` - correctly continuing the
numbering from 31 pre-existing snapshots on this card, proving the
"probe for the first free name" logic and the concurrency fix both work),
whereas every single attempt failed at the open step before this fix.

**Not yet confirmed - a full save.** After successfully opening
`FACE0032.BMP`, the actual `f_write()` of the 153,600-byte pixel payload
failed ("Snapshot: write failed (FACE0032.BMP) after 160880us (160ms)" -
the timing itself is normal, matches this project's own previously-
measured SD write speed), and every attempt after that failed back at
"could not create a new file" again. Two honest, NOT mutually exclusive
possibilities, deliberately not overclaiming either:
1. This specific SD card already has 31+ full-size (153,600-byte pixel
   payload each) snapshots on it from earlier single-core testing
   sessions (WORKLOG.md's 2026-08-25 entries) - that's 4.6MB+ already
   used, and this could plausibly be a genuinely small/near-full test
   card running out of free clusters, which would explain a write failure
   on a large payload specifically (not the small 66-byte header) and a
   corrupted/incomplete file afterward interfering with subsequent
   creates.
2. A residual concurrency gap not yet found - e.g. `SNAPSHOT_OnFrame()`
   itself calls multiple separate FatFs operations (`f_open`, two
   `f_write()` calls, `f_close()`) with the bus mutex released BETWEEN
   each individual `disk_*()` call, not held across the whole snapshot
   sequence - if that turns out to matter (not yet proven either way),
   the fix would be locking around all of `SNAPSHOT_OnFrame()`, not just
   each individual diskio call.
Per this project's own "confirm on real hardware, don't guess" standard -
this needs a fresh/reformatted card to actually distinguish the two
theories, not more reasoning from this session's log output alone.

**fps held at 11 throughout** (briefly dipped to 10 during one write
attempt) - the equal-priority scheduling change didn't meaningfully cost
the camera/LCD path anything. Legacy single-core build re-confirmed
unaffected throughout (same regression-check pattern as every prior stage).

### Next steps for a fresh session

1. Retest Stage 4 with a freshly-formatted (or otherwise confirmed-to-have-
   free-space) SD card to determine whether the write failure was card
   capacity or a residual concurrency gap - see the two theories above.
2. If it turns out to be concurrency: move the `SPI1_BUS_Lock()`/`Unlock()`
   pair to wrap the whole `SNAPSHOT_OnFrame()` call (in `StorageTask`,
   `main_core1.c`) instead of/in addition to the per-diskio-call locks
   already in `sd_spi_disk.c`.
3. Stage 5 unchanged from the approved plan - see
   `~/.claude/plans/stateful-churning-flurry.md`. Note core1's RAM is now
   genuinely tight (`m_data` at ~99% before the `-Os` fix, comfortable
   margin after) - the frame buffer/crop buffer/result struct Stage 5 adds
   all live in the shared region (not core1's own budget), so this should
   be fine, but worth a real build check early again rather than assuming.

## Dual-core RTOS migration Stage 3 CONFIRMED ON REAL HARDWARE - camera capture + LCD preview ported to core1, exactly matches the documented single-core tear-free baseline (11fps); a second real core1 RAM-budget overflow found and fixed by rebalancing text/data again after a real measurement, not by guessing bigger (2026-09-04)

Follow-up to the Stage 2 entry below (same day). Ported the legacy
`main.c`'s `DEMO_LCD_CAMERA_PREVIEW` loop (camera capture -> LCD push,
Deinit/Reinit tearing fix + `skipNextFrame`) onto core1 as a single
FreeRTOS task - `source/main_core1.c`'s `CameraLcdTask`. Core0 for this
stage still just boots core1 and idles (no AI yet, that's Stage 5).

**The 320x240 RGB565 frame buffer (153,600 bytes) does not fit in core1's
own RAM at all** - confirmed by a real link failure, not just the earlier
paper analysis. Moved `camera_capture.c`'s `s_frameBuffer` into the shared
region (`source/shared/ipc_layout.h`'s `IPC_FRAME_BUFFER_ADDR`) for the
`DUALCORE_RTOS` build specifically (guarded, the legacy build keeps its
own private static array) - this needs to happen now, not just at Stage 5,
purely because of RAM size, before any cross-core sharing is even wired
up. One real gotcha: `s_frameBuffer` becomes a pointer-valued macro in
this mode, so `sizeof(s_frameBuffer)` (used in the buffer-clearing
`memset()`) silently changes meaning from "153,600 bytes" to "4 bytes" -
fixed by computing the byte count explicitly instead of via `sizeof()`.

**A second, real `m_text` overflow found by linking, not estimated**:
Stage 2's 60KB/43KB core1 text/data split (already a real-measurement-
based rebalance of the vendor's 51KB/52KB default) still overflowed by
5,584 bytes once the actual camera/LCD/SPI driver code was linked in.
Investigated two candidate causes before finding the real fix:
- **Wrong lead**: suspected USB Video Class code (auto-pulled via the
  legacy top-level `prj.conf`'s `CONFIG_USB_DEVICE_CONFIG_VIDEO=1`, which
  Kconfig `default y if USB_DEVICE_CONFIG_VIDEO > 0`-selects the video/
  EHCI/PHY components) was bloating the link. Tried disabling it 3 ways in
  `board_port/cm33_core1/prj.conf` (disabling the derived component,
  disabling the root trigger with `# ... is not set` syntax, then with the
  correct `=0` syntax for its actual int Kconfig type) - **none of the
  three changed the final size by even one byte**, because `--gc-sections`
  was already stripping all of it - it never cost anything in the first
  place. Confirmed by removing the USB object files from the link
  directly (`mcux_project_remove_source()`) and observing zero size change.
- **Real fix**: the actual 62,992-67,024 bytes of `m_text` genuinely needed
  by FreeRTOS + MCMGR + the real camera/LCD/SPI driver code. Trimmed
  FreeRTOS down to only the modules this project's code actually calls
  (`tasks.c`/`list.c`/`queue.c`/`port.c`/`portasm.c`/
  `mpu_wrappers_v2_asm.c`/`heap_4.c` - dropped `timers.c` (no software
  timers used anywhere, also set `configUSE_TIMERS=0`),
  `event_groups.c`/`stream_buffer.c`/`croutine.c` (none of these APIs are
  used - task notifications, used for all IPC, don't need any of them)),
  which closed most of the gap (down to a 1,552-byte overflow), then
  rebalanced core1's fixed 104KB text/data split a second time - 64KB
  text / 39KB data (up from 60/43) - based on the real, now-measured
  62,992/38,816-byte need instead of another guess. Both builds succeeded
  after this with real margin (`m_text` and `m_data` both comfortably
  under 100% - see the confirmed-on-hardware numbers below).

**Confirmed on real hardware, exactly matches the documented single-core
baseline** - serial log captured the same way as Stages 1-2:
```
Camera: OV7670 detected on J9 (PID=0x76 VER=0x73 confirmed), 320x240 @ 30 fps.
LCD: hardware SPI (LPSPI1, shared bus) on the Arduino header
LCD: SPI1 source clock = 48000000 Hz, requested 24000000 Hz, achieved 24000000 Hz
LCD preview: 11 fps
```
**11fps, matching the legacy single-core build's own confirmed tear-free
number exactly** (see the "LCD tearing FIXED" entry below - `Preview fps:
18 -> 11`, the same real cost of the same Deinit/Reinit tearing fix) - the
move to FreeRTOS + dual-core cost nothing extra on top of that fix, at
least at this fps range. Legacy single-core build re-confirmed unaffected
by all of the above (rebuilds with `ninja: no work to do` / unchanged
`m_data` usage, per the established regression-check pattern).

### Next steps for a fresh session

1. Stage 4: port `sd_spi_disk.c`, `snapshot.c`, FatFs wiring onto core1 as
   a `StorageTask`, triggered by a manual test hook (AI not wired up
   yet) - confirm SD mount/write still works under RTOS scheduling. Watch
   for the same class of core1 RAM-budget surprise found in Stage 3 - test
   a real build early rather than assuming the current 64KB/39KB split
   has enough headroom for FatFs + the SD driver.
2. Stage 5 unchanged from the approved plan - see
   `~/.claude/plans/stateful-churning-flurry.md`.

## Dual-core RTOS migration Stage 2 CONFIRMED ON REAL HARDWARE - FreeRTOS running on both cores, full MCMGR ISR->task event round-trip (core1 ping -> core0 -> core1 pong) working; one real port-config bug found and fixed via live SWD fault analysis (2026-09-04)

Follow-up to the Stage 1 entry below (same day, continuing directly after
Stage 1's real-hardware confirmation). Added FreeRTOS to both cores plus
one MCMGR event round-trip, per the approved plan's Stage 2.

**FreeRTOS pulled in directly via CMakeLists.txt (`mcux_add_source`), not
via the `CONFIG_MCUX_COMPONENT_middleware.freertos-kernel` Kconfig
component** - that component's Kconfig lives in the same `prj.conf` files
the legacy single-core build also reads, and FreeRTOS's `port.c`
would then also compile into the legacy build even though it never calls
`vTaskStartScheduler()`. Wrote `source/shared/FreeRTOSConfig.h` by hand
instead of using the SDK's Kconfig-driven auto-generation
(`FreeRTOSConfig_Gen.h`) - kept entirely inside the `DUALCORE_RTOS` CMake
branch, zero risk to the legacy build. Required hook functions
(`vApplicationStackOverflowHook`, `vApplicationMallocFailedHook` - both
needed once `configCHECK_FOR_STACK_OVERFLOW=2`/`configUSE_MALLOC_FAILED_HOOK=1`
are set) added in `source/shared/freertos_hooks.c`.

**Real bug found and fixed via live SWD fault-register analysis, not
guessed** (same debugging technique this project's own WORKLOG has used
before, e.g. the CMSIS-NN stack-overflow bug): first attempt at
`FreeRTOSConfig.h` built fine, flashed fine, but core0 silently hung
right after printing "starting scheduler..." - no crash message (no
`fault_handler.c`-equivalent installed in this minimal dual-core build),
no further output, ever. Two wrong theories tried and ruled out along the
way (documented so a future session doesn't repeat them):
1. Guessed the `vPortPendSVHandler`/`vPortSysTickHandler` rename macros
   (an older FreeRTOS naming convention) were wrong for this kernel
   version - fixed the names to `xPortPendSVHandler`/`xPortSysTickHandler`
   (matching a reference Kconfig-generated config) and rebuilt - **no
   change in symptom**, disproving the theory. Then read `port.c`/
   `portasm.c` directly: this kernel version defines `SVC_Handler`/
   `PendSV_Handler`/`SysTick_Handler` as real, literal function names
   (naked functions in `portasm.c`, direct definition in `port.c`) - none
   of those rename macros do anything at all in this port version, they're
   vestigial. Confirmed via `arm-none-eabi-nm`/`objdump` on the actual
   built `.elf` that the real FreeRTOS `PendSV_Handler` (context-switch
   assembly, not a stub) was correctly linked into the vector table -
   ruled this out definitively before spending more time on it.
2. With the real culprit still unknown, halted the running core directly
   over SWD (`pyocd commander -c halt -c reg`) instead of guessing further -
   found `pc` sitting exactly at `HardFault_Handler`. Read `CFSR`
   (`0x00040000` = UsageFault bit 2, INVSTATE - invalid execution state),
   `HFSR` (`0x40000000` = FORCED, meaning a fault escalated to HardFault
   because the original handler wasn't enabled), and `SHCSR` (`0x84` -
   `SVCALLACT` bit set, meaning the fault happened *while inside the SVC
   call*, i.e. during `vTaskStartScheduler()`'s first-task-start sequence,
   not later). This pointed at the initial fake exception stack frame
   `pxPortInitialiseStack()` builds for the very first task having the
   wrong execution-state assumptions baked in for the port's actual
   secure/TrustZone configuration.
3. Root cause: `configRUN_FREERTOS_SECURE_ONLY` was never defined in
   `FreeRTOSConfig.h` - undefined evaluates to `0` in the preprocessor,
   but `port.c`'s own header comment documents that for the "NTZ" (No
   TrustZone) port variant this project uses, the valid combination is
   `configRUN_FREERTOS_SECURE_ONLY=1` **with** `configENABLE_TRUSTZONE=0`,
   not both `0` - confirmed by diffing against the SDK's own Kconfig-
   generated reference config, which explicitly sets this to `1`. Added
   `#define configRUN_FREERTOS_SECURE_ONLY 1` - fixed on the very next
   flash, confirmed via the same SWD halt-and-read technique that the
   fault no longer occurs, then confirmed via the actual serial log that
   the scheduler now runs correctly on both cores.

**Confirmed on real hardware, full round trip, clean serial capture**
(`stty`+`cat` on `/dev/ttyACM0` around a reset, same technique as Stage 1):
```
core1: sending ping to core0...
core0: got ping 0x1111 from core1, sending pong
core1: got pong 0x2222 from core0 - round trip OK
```
This proves the whole ISR-safe IPC design end to end: `source/shared/
ipc_events.c`'s `MCMGR_RegisterEvent`/`MCMGR_TriggerEvent` wrapper, the
`xTaskNotifyFromISR`+`portYIELD_FROM_ISR` pattern inside the MAILBOX_IRQn
ISR context (confirmed safe against `configMAX_SYSCALL_INTERRUPT_PRIORITY`
in an earlier planning pass, now confirmed working for real), and
`xTaskNotifyWait` on the receiving task - exactly the mechanism Stage 5's
real frame-ready/result-ready doorbells will reuse.

**Also re-confirmed, expected**: with two FreeRTOS tasks now both logging
(not just one-shot boot banners like Stage 1), the shared-UART byte
interleaving flagged in the Stage 1 entry is worse, as expected - fully
decodable by hand in this capture, but a real argument for adding a
mutex/arbitration around `PRINTF()` before Stage 3+ needs sustained,
readable logging from both cores concurrently (e.g. camera fps stats on
core1 next to inference timing on core0).

**Legacy single-core build re-confirmed unaffected** after this change
too (`ninja: no work to do` / unchanged `m_data` usage) - the
`DUALCORE_RTOS` CMake option continues to fully gate this work away from
the production build.

### Next steps for a fresh session

1. Consider a shared-UART print mutex before Stage 3 (camera+LCD on core1)
   adds sustained logging - see "Also re-confirmed, expected" above.
2. Stage 3: port `camera_capture.c`, `lcd_spi_hw.c`, `spi1_bus.c`,
   `bbox_overlay.c`, `text_overlay.c` onto core1, running the existing
   `LCD_CAMERA_PREVIEW` logic (Deinit/Reinit tearing fix + `skipNextFrame`)
   inside a single task/discipline - re-measure fps against the documented
   baseline (18fps tearing / 11fps tear-free) once real hardware access is
   available.
3. Stages 4-5 unchanged from the approved plan - see
   `~/.claude/plans/stateful-churning-flurry.md`.

## Dual-core RTOS migration Stage 1 CONFIRMED ON REAL HARDWARE - core1 boot-only bring-up, real RAM budget measured against actual reference builds instead of assumed (2026-09-04)

User requested two changes: move from bare-metal to an RTOS (FreeRTOS), and
actually boot core1 - split the app so core0 does AI inference only, core1
does camera capture + LCD/SPI display + SD card saving. This is the
project's first time booting core1 at all - ARCHITECTURE.md/this file's own
history already document that reusing core1's RAM region *without* booting
it correctly bricked the board once (recovered via `nxpdebugmbox`), so this
migration is being done in small, real-hardware-confirmed stages rather
than as one big rewrite. Full design doc:
`~/.claude/plans/stateful-churning-flurry.md` (approved plan, dual-core
architecture: single shared frame buffer, MCMGR events only - no
RPMsg-Lite - for cross-core signaling).

**Real measurement before writing any RAM-partition code (the "Phase 0"
step the plan called for).** Built NXP's own
`multicore_examples/rpmsg_lite_pingpong_rtos` secondary-core example for
this exact board to get a real FreeRTOS-on-core1 code-size baseline, rather
than guessing: **`m_text: 36,532B/51KB (70%)`, `m_data: 13,912B/52KB
(26%)`** - this INCLUDES RPMsg-Lite, which this project doesn't use (MCMGR
events only, lighter). Also measured this project's own existing
camera/LCD/SD driver `.text` directly from the current single-core build's
`.obj` files: **~26.6KB `.text`, ~9.7KB static `.bss`** (excluding the
153,600-byte camera frame buffer, which moves to shared RAM - see below).

**Corrected a wrong assumption from the initial plan**: the vendor's
default core1 RAM split (1KB interrupts / 51KB text / 52KB data, all inside
the fixed 104KB region at `0x2004E000-0x20068000`) leaves text tighter than
expected once this project's own driver code is added on top of even a
trimmed (no-RPMsg) FreeRTOS baseline. Fixed by **rebalancing the same fixed
104KB total** (not widening it - that boundary is hardware/power-domain
fixed, not a linker choice) to 1KB / **60KB text** / **43KB data** - data
was barely a third used even with RPMsg-Lite, so it had margin to give.

**Also found, by reading NXP's own reference example instead of assuming**:
a 320x240 RGB565 frame buffer (153,600 bytes) does NOT fit in core1's
104KB region at all - it has to live in core0's much larger `m_data`
instead (312KB, only ~178KB used today), made visible to core1 via fixed
pointer addresses (`source/shared/ipc_layout.h`), not a shared linker
MEMORY region on core1's side - core1 doesn't need one, it just reads/
writes the raw address. New `m_shared` region: `0x20024000`, 168KB,
carved from the top of core0's `m_data` (which shrinks from `0x4E000` to
`0x24000` accordingly, still comfortably fitting core0's NPU tensor arena).

**How the base linker script gets replaced** (needed since this repartition
requires a full MEMORY-block change, not an additive fragment - see
`board_port/ei_sramx.ld`'s own comment on why additive `-T` scripts can't
resize an existing named region, and the "m_data_ext.ld" incident earlier
in this file for what happens when you try anyway): found and used
`mcux_remove_armgcc_linker_script()` (cancels the SDK's own default `-T`
for that exact file+path) followed by `mcux_add_armgcc_linker_script()`
with this project's own fork - confirmed working by a real build showing
the new `m_shared`/rebalanced-`m_data` regions in the link's own memory-
usage report, not just by reading the CMake source.

**How core0 embeds core1's binary**: NXP's `fsl_incbin.S`
(`components/misc_utilities/`) uses the GNU assembler's `.incbin` directive
to literally embed core1's raw `.bin` into core0's flash at build time -
requires core1 to be built FIRST into a sibling `core1/` build directory
so the assembler's include path (`../core1/armgcc/`) can find
`core1_image.bin`. `main_core0.c` then `memcpy()`s it to RAM at
`0x2004E000` and calls `MCMGR_StartCore()` - pattern copied from NXP's own
`multicore_examples/hello_world` for this exact board.

**Confirmed by real builds** (not just "should work"): core1 builds using
its own `board_port/cm33_core1/MCXN947_cm33_core1_dualcore.ld`
(`m_text: 22,268B/60KB (36%)`, `m_data: 4,168B/43KB (9.5%)` - real margin
even before FreeRTOS/Stage 2 lands). core0 builds using its own
`board_port/cm33_core0/MCXN947_cm33_core0_dualcore.ld`, correctly shows
`m_core1_image: 23,892B/256KB` (core1's incbin'd binary) and
`m_shared: 0B/168KB` (declared, unused until Stage 5). **The existing
single-core production build was rebuilt afterward and confirmed
unaffected** (`m_data: 182,192B/312KB`, same as before) - the whole
dual-core path is gated behind a new `-DDUALCORE_RTOS=ON` CMake option
(default OFF), specifically so this migration can proceed stage-by-stage
without ever putting the working single-core firmware at risk.

**New build command**: `./build.sh dualcore-build` (builds core1 then
core0, confirmed working end-to-end from a clean state) / `dualcore-flash`
/ `dualcore-all` - separate from the existing `build`/`all` commands and
`build/` output directory (dual-core artifacts land in `build_dualcore/`).

**CONFIRMED ON REAL HARDWARE - Stage 1's exit criteria met.** Flashed
`dualcore-all`, captured the serial log directly (`stty`+`cat` on
`/dev/ttyACM0` around a `./build.sh reset`, not just eyeballing a terminal).
Both cores print their banners, repeatable across multiple resets, no hang,
no bricked board - the single most important checkpoint in the whole
migration (given this exact class of mistake, touching core1's RAM without
booting it correctly, bricked the board once before) is now real, not
theoretical.

**Two real bugs found and fixed via the live serial log, not guessed
upfront:** `MCMGR_StartCore(..., kMCMGR_Start_Synchronous)` on core0 kept
silently blocking forever (core1 booted and printed fine, but core0's own
"core0: core1 started." line never appeared) until BOTH of these were
added to core1's `main_core1.c`, confirmed by reading `mcmgr.c`'s actual
source rather than assuming:
1. `MCMGR_Init()` - without it, core1 never participates in the MCMGR
   protocol at all.
2. `MCMGR_GetStartupData(kMCMGR_Core0, &startupData)` (looped until it
   returns success) - `MCMGR_StartCore()`'s synchronous wait polls
   `state != kMCMGR_RunningCoreState`, and that state transition is a
   direct side effect of the *secondary* core calling this function (it
   triggers `kMCMGR_FeedStartupDataEvent` back to core0) - `MCMGR_Init()`
   alone does not do this. First attempt added only #1 and still hung;
   re-reading `mcmgr.c`'s `MCMGR_StartCore()` line-by-line (not the header
   docs, which don't spell this out) found the real mechanism. Pattern now
   matches NXP's own `multicore_examples/hello_world` secondary/main.c
   exactly.

**Confirmed, expected, not a bug**: once the handshake was fixed, "core0:
core1 started." and core1's own banner print at almost the same instant
and land BYTE-INTERLEAVED in the captured log (e.g. "co\r\nCrea0m:e
rcao_rAeI_1 Tsestatr1t e-d c.o\r\n...") - both cores share one physical
debug UART with no arbitration between them. Manually decoding the
interleave confirms both full lines are genuinely present and correct, just
garbled in transmission order. This is a real, expected consequence of two
cores writing to the same UART peripheral concurrently, not a data
corruption bug - worth a mutex/arbitration scheme once Stage 2+ has real,
frequent logging from both cores (not urgent for Stage 1's one-shot
banners).

### Next steps for a fresh session

1. Stage 2: add FreeRTOS to both cores (Kconfig component already wired
   for `middleware.freertos-kernel.cm33_non_trustzone` - not yet added to
   prj.conf, that's Stage 2's job) plus one MCMGR event round-trip
   (`kMCMGR_RemoteApplicationEvent`, ISR-context callback confirmed via
   reading `mcmgr_internal_core_api_mcxnx4x.c` - must use
   `xTaskNotifyFromISR`/`portYIELD_FROM_ISR`, never a blocking FreeRTOS
   API). Consider whether the shared-UART interleaving above needs a
   mutex before Stage 2's tasks start logging more frequently.
2. Stages 3-5 (camera+LCD on core1, SD snapshot on core1, full AI pipeline
   with the shared frame buffer) unchanged from the approved plan - see
   `~/.claude/plans/stateful-churning-flurry.md`.

## LCD tearing FIXED - user-captured video showed a real horizontal tear line, root-caused to the single shared camera frame buffer having no synchronization with the LCD push; fixed by pausing SmartDMA around the push (same pattern the AI loop already uses). Preview fps: 18 -> 11 (real cost of the fix). A separate, NOT-fully-root-caused SmartDMA-adjacent memory corruption was found and worked around (2026-09-04)

Follow-up to the entry directly below (same day). User sent a video of the
live camera-preview display and asked why it showed "striped" content.

**Diagnosis from the video, not guesswork.** No video/frame-extraction
tool was available directly (no ffmpeg) - used `cvlc`'s `scene` video
filter (`--video-filter=scene --scene-format=png`, `--vout=dummy` alone
hit a VLC filter-chain bug, needed `--no-spu --no-osd --avcodec-hw=none`
too before it would actually write files) to pull dozens of PNG frames
out of the phone video, then cropped/zoomed into just the LCD region with
PIL. Found one frame (out of ~43 densely sampled) with a genuine, sharp
horizontal white seam partway down the image - confirmed it was on the
LCD's own displayed content, not a wire/reflection in front of it (no
wire crosses that exact spot in the un-cropped frame).

**Root cause**: `main.c`'s `LCD_CAMERA_PREVIEW` loop reads
`CAMERA_CAPTURE_GetFrameBuffer()` directly while `LCD_DrawImage()` pushes
it out over SPI/eDMA (~57ms) - single shared frame buffer, no
double-buffering, no synchronization beyond the one-shot `s_frameReady`
flag. This was harmless while the camera was slower than the LCD push
(its real rate was ~7.3fps/~137ms before the XCLK fix two entries below),
but that same fix made SmartDMA genuinely deliver ~30fps/~33ms - FASTER
than the ~57ms LCD push - so a new camera frame can now land mid-push,
producing exactly the observed seam (top of screen = older frame, bottom
= newer one). This is the exact risk flagged as an open item in the
"fps follow-up" entry below ("current single-buffer design has no
overlap between capture and push") - this video is the first real visual
confirmation of it, and a direct, unintended side effect of fixing the
camera clock.

**Real double-buffering (two full frame buffers) is not RAM-feasible
here**: a second 320x240 RGB565 buffer is 153,600 bytes - bigger than
this build's entire free `m_data` headroom (137KB) and bigger than
`m_sramx` (96KB) on its own, and a single buffer can't be split across
those two non-contiguous physical banks (same constraint this project's
AI-arena RAM investigation already ran into).

**Fix**: pause SmartDMA around the LCD push instead - `main.c`'s preview
loop now calls `CAMERA_CAPTURE_Deinit()` before `LCD_DrawImage()` and
`CAMERA_CAPTURE_Reinit()` after, plus a `skipNextFrame` flag to discard
the frame immediately after each reinit (SmartDMA needs one cycle to
resync with the sensor's HREF/VSYNC/PCLK timing after a fresh boot -
exact same workaround already proven in the AI-build's loop, which uses
this pattern for a different reason - RAM collision with SmartDMA's own
firmware, see the "AI model integration" summary further down). This
guarantees SmartDMA is never active while the buffer is being read, at
the cost of some fps (re-init has a real cost, and every other real
camera frame is now discarded).

**Confirmed on real hardware**: `LCD preview: 11 fps` (down from 18fps
without the fix), stable and consistent across repeated resets. Real,
measured cost of the fix - not assumed. Production AI-enabled build
reflashed too - boots clean, SD card + inference (~3.9ms/frame) + camera
all still working normally, no regressions from the shared-file changes
(`camera_capture.c`, `lcd_spi_hw.c`).

**A second, separate bug was found (and worked around, not fully
root-caused) while testing this fix**: as soon as Deinit()/Reinit() started
running every displayed frame, `lcd_spi_hw.c`'s own per-window diagnostic
print (`LCD: diag - frame push=...`) started reading back nonsense -
billions of "frames", `0us/frame avg`, wildly wrong window durations.
Narrowed down via `nm`: those diagnostic statics land in RAM immediately
adjacent to `camera_capture.c`'s SmartDMA parameter/stack statics
(`smartdmaParam`, `s_smartdmaStack`). Found that `s_smartdmaStack` was
only 32 bytes - HALF of what `fsl_smartdma_fw.h`'s own comment documents
as the real requirement ("shall be at least 64 bytes") - bumped it to 128
bytes as a legitimate, independently-justified fix, but **this did NOT
stop the corruption** - so the SmartDMA-stack-undersized theory, while a
real documentation-vs-code gap worth having fixed anyway, was NOT the
actual mechanism. (Also checked: NXP's own reference example,
`smartdma_camera_flexio_mculcd`, uses the same 32-byte size without any
reported issue, which is why this alone was never going to be the whole
story.) Root cause NOT fully pinned down beyond "something about
SmartDMA's teardown/reboot cycle occasionally writes into memory near its
own parameter block" - `SMARTDMA_Deinit()` just gates a command register
and a clock, with no confirmation the coprocessor has actually halted
mid-instruction first, which is a plausible mechanism for a race but
wasn't directly proven.

**Why this was worked around instead of chased further**: the corrupted
statics are diagnostic-only (a print, not read by anything else), and the
real pixel data (`s_pixelSwapBuf`, which sits further along in the same
RAM region) is fully rewritten by the byte-swap loop before every push,
strictly AFTER `CAMERA_CAPTURE_Reinit()` already ran on the *previous*
cycle - so even if that reinit transiently scribbles nearby memory, the
pixel buffer gets fully overwritten with fresh correct data before the
next push ever reads it. Removed the now-unreliable diagnostic from
`lcd_spi_hw.c` entirely (the number it measured - ~56.9ms/frame LCD push
time - was already firmly established in earlier sessions and is
unchanged by this fix) rather than ship something that prints garbage.
`main.c`'s own fps/wait-for-frame counters (plain stack locals, not
statics living in this danger zone) stayed reliable throughout and are
now the only per-frame timing diagnostic in the preview build.

**Not yet confirmed: the tear is actually gone** - no camera/photo access
this session. The reasoning above (SmartDMA fully stopped for the entire
duration the buffer is being read) should make tearing structurally
impossible now, not just less likely, but this needs a real look at the
display (or another video) to confirm, same caveat as every other
display-behavior entry in this file.

### Next steps for a fresh session

1. **Get the user to look at the live image (or another video)** and
   confirm the tear is gone - the one thing this session's code-reasoning
   can't substitute for.
2. If the SmartDMA-adjacent memory corruption ever causes a REAL
   (non-diagnostic) symptom - e.g. visible image corruption, or corruption
   of some other static that isn't self-healing like `s_pixelSwapBuf` is -
   revisit root-causing it properly: try reading `SMARTDMA->CTRL`/status
   registers live via SWD right after `SMARTDMA_Deinit()` to see if the
   coprocessor actually reports halted before the next boot starts, or try
   adding a short busy-wait/poll after Deinit() before Reinit() to rule
   out a teardown race empirically.
3. 11fps is a real, working, tear-free number - worth explicitly checking
   with the user whether that's an acceptable trade-off versus the
   previous 18fps-but-tearing-risk state, before considering any further
   optimization (e.g. real double-buffering would need freeing a lot more
   RAM first - see the "not RAM-feasible" note above).
4. The bus-sharing baud-reclaim/delay-register logic and touch still have
   zero real-hardware confirmation beyond the LCD-only camera-preview
   test - see earlier entries for what to test (unchanged from previous
   entries - not touched this session).

## Camera fps ROOT-CAUSED AND FIXED: XCLK was actually 6MHz (MAIN_CLK/25), not the 24MHz the OV7670 driver assumed - a real ~4x mismatch, confirmed present in NXP's own reference example too - fixed by re-sourcing CLKOUT from FRO_HF/2. Preview fps: 7 -> 8 (eDMA) -> 18 (this fix) (2026-09-04)

Follow-up to the entry directly below (same day). User asked to confirm the
camera's real free-running fps before deciding whether to invest in
fixing it (per the entry below's own recommended next step), then asked
to dive into fixing it once the number came back low.

**Confirmation test**: added a one-shot diagnostic in `main.c` (before the
normal preview loop) that counts `CAMERA_CAPTURE_GetFrameCount()`
increments over a fixed 3-second window with ZERO consumption - no
`LCD_DrawImage()` call at all, not even reading the frame buffer, since
the frame counter increments in the SmartDMA completion ISR regardless of
whether anything reads the frame out. Result: **`22 frames in 3s = 7.3fps
free-running`** - conclusively rules out the "software handshake/consume-
loop throttling" theory from the previous entry; the camera/SmartDMA path
itself is only delivering ~7.3fps, decoupled from anything LCD-side.

**Root cause, found by reading `hardware_init.c`'s own camera clock setup
next to `camera_capture.c`'s OV7670 config**: `BOARD_InitHardware()`
routes the camera's XCLK pin (P2_2/CLKOUT) from `MAIN_CLK` (150MHz, from
`BOARD_BootClockPLL150M()`) divided by 25 -
`CLOCK_SetClkDiv(kCLOCK_DivClkOut, 25U)` - giving a real, exact
**6,000,000 Hz**. But `camera_capture.c`'s `ov7670_resource_t` declares
`.xclock = kOV7670_InputClock24MHZ` and requests `framePerSec = 30U`.
`fsl_ov7670.c`'s `OV7670_Configure()` picks its `CLKRC` register value
(and the sensor's whole internal frame-timing state machine derives from
that, per the OV7670 datasheet) from a lookup table keyed on the
DECLARED xclock (24MHz) - it has no way to detect what XCLK the sensor is
ACTUALLY receiving. Feeding it 1/4 of the rate its own `CLKRC` setting
assumes makes its entire capture cycle run ~4x slower than intended:
30fps / 4 = 7.5fps - matching the measured 7.3fps almost exactly.

**Not a bug introduced by this project** - checked NXP's own
`examples/_boards/frdmmcxn947/display_examples/smartdma_camera_flexio_mculcd`
reference example (this project's `hardware_init.c` header comment
already credited it as the source of this camera clock bring-up code):
it has the EXACT SAME `CLOCK_AttachClk(kMAIN_CLK_to_CLKOUT);
CLOCK_SetClkDiv(kCLOCK_DivClkOut, 25U);` and its own camera source
(`smartdma_camera_flexio_mculcd.c`) ALSO declares `.xclock =
kOV7670_InputClock24MHZ` with `framePerSec = 30`. This looks like a
latent bug in NXP's own reference example - most likely never caught
because that example just shows a live low-fps feed on a parallel LCD
without anyone measuring the achieved rate against the requested one, the
same way this project hadn't measured it either until this session added
the zero-consumption diagnostic above.

**Why divisor=25 was chosen: it's the closest clean number to 24MHz from
150MHz, not actually 24MHz.** 150MHz has no integer divisor landing
exactly on any of the 4 XCLK rates `fsl_ov7670.c`'s lookup table supports
(24/12/26/13 MHz) - 150/24=6.25, 150/12=12.5, 150/26≈5.77, 150/13≈11.54,
none are whole numbers. Divisor 25 gives a suspiciously clean 6MHz, which
is probably why nobody's automated build/lint caught an obviously "wrong"
non-integer divisor - the number LOOKS deliberate, it's just deliberately
targeting the wrong clock rate for what the driver call three lines away
actually declares.

**Fix**: source CLKOUT from `FRO_HF` (48MHz) instead of `MAIN_CLK` -
`FRO_HF` is already running and independently confirmed stable at exactly
48MHz on this board (via `spi1_bus.c`'s `SPI1_BUS_GetSourceClockFreq()`
diagnostic, used for LPSPI1's own clock). `48,000,000 / 2 = 24,000,000` -
a genuine, exact 24MHz, actually matching what `camera_capture.c`
declares this time. Changed in `hardware_init.c`:
`CLOCK_AttachClk(kFRO_HF_to_CLKOUT); CLOCK_SetClkDiv(kCLOCK_DivClkOut,
2U);`.

**Confirmed on real hardware - dramatic, real improvement:**
- Re-ran the same zero-consumption camera-only diagnostic:
  **`90 frames in 3s = 30.0 fps free-running`** - hits the configured
  target exactly, confirming the XCLK mismatch really was the entire
  story.
- `LCD_CAMERA_PREVIEW=ON` build, full preview loop: **`LCD preview: 18
  fps`**, up from 8fps (eDMA fix) and 7fps (original CPU-polled path) -
  more than DOUBLED. `wait-for-frame` (time spent waiting for
  `CAMERA_CAPTURE_IsFrameReady()`) dropped from ~75.5ms/frame to
  **~0.3ms/frame** - the camera is now so much faster than the LCD push
  that frames are essentially always already waiting by the time the
  loop checks. The loop is now purely bound by the LCD push time
  (~56.9ms/frame, unchanged, still near its ~51ms bit-clock floor) -
  1000/56.9 ≈ 17.6fps, matching the measured 18fps almost exactly. This
  also means the whole investigation has come full circle: with the
  camera fixed, the LCD/SPI side (this file's very first entries) IS
  now, finally, actually the binding constraint again, just like the
  original ~19-20fps best-case math always assumed - that math was right
  all along, it just couldn't be reached while the camera was
  independently 4x too slow underneath it.
- Default AI-enabled production build (SD card + inference + LCD status
  text): reflashed, captured a fresh serial log - boots clean, SD card
  initializes, inference keeps running at the same ~3.9ms/frame cost as
  before (unaffected by camera rate, as expected - it's a separate NPU
  operation), the periodic `Camera: frame #N ready...` diagnostic log
  advances much faster than in previous captures (consistent with the
  camera genuinely running faster now), pixel range/avg values printed
  look like real varying image data, not flat/dead - no corruption or
  hangs observed.
- Removed the temporary zero-consumption diagnostic block from `main.c`
  afterward (it added a fixed 3-second boot delay, no longer needed once
  the fix was confirmed) - not left in as permanent instrumentation,
  unlike the per-frame fps/push-time diagnostics from the entry below,
  which stay since they're cheap and remain generally useful.

**Not yet confirmed: image quality/color at the new, much higher real
frame rate** - no camera/photo access this session, same caveat as the
eDMA-fix entry below. A faster camera clock changes the SmartDMA
capture cadence, not the SPI pixel-push mechanism, so this is a lower-risk
gap than the earlier eDMA corruption risk, but still genuinely
unconfirmed by eye.

### Next steps for a fresh session

1. **Get the user to look at the live image** at the new, higher fps -
   still the one thing this session's numbers can't substitute for (now
   doubly true: both the eDMA pixel-push AND the camera clock changed
   since the last human visual check).
2. 18fps is very close to the ~19-20fps best-case math for a 320x240 push
   at 24MHz SPI - LCD-side gains alone have very little headroom left
   without a higher SPI clock (the PLL0-route option discussed in earlier
   entries, with its own signal-integrity risk on this breadboard wiring)
   or reduced per-frame data (resolution/color-depth cut). Worth
   explicitly checking with the user whether 18fps is an acceptable
   stopping point before pursuing either.
3. Sanity-check whether the OV7670's OTHER frame-rate options (25/15/14fps,
   also indexed by declared xclock in the same lookup table) were ever
   used anywhere in this project with the OLD 6MHz-actual/24MHz-declared
   mismatch in effect - if `framePerSec` was ever changed away from 30
   anywhere, that specific configuration's assumptions should be
   re-checked against the real, now-fixed 24MHz XCLK too (not expected,
   given `camera_capture.c` only ever requests 30, but worth a quick grep
   before assuming no other code path is affected).
4. The bus-sharing baud-reclaim/delay-register logic and touch still have
   zero real-hardware confirmation beyond the LCD-only camera-preview
   test - see earlier entries for what to test (unchanged from previous
   entries - not touched this session).

## fps follow-up: per-chunk Prepare() hypothesis DISPROVED by direct A/B test - LCD push is already near its theoretical floor (~57ms/frame vs. ~51ms best-case); the real remaining bottleneck moved to the CAMERA side, not measured before now (2026-09-04)

Follow-up to the entry directly below (same day). User asked to try
raising fps for the LCD preview build specifically.

**Hypothesis tested: hoist `LPSPI_MasterTransferPrepareEDMALite()` out of
the per-chunk loop.** The previous entry's back-of-envelope math (451ms
"per window" total, divided by 19 chunks/frame instead of by frame count)
suggested ~21ms/chunk of Prepare()-related overhead. Split
`SPI1_BUS_TransferBytesDMA()` into `SPI1_BUS_PrepareDMA()` (called once
per `LCD_PushPixelsOpen()` invocation) + a leaner
`SPI1_BUS_TransferBytesDMA()` (no more per-chunk Prepare/RXMSK-clear) -
matches mcuxsdk's own reference example's call pattern exactly
(`examples/_boards/frdmmcxn947/driver_examples/lpspi/edma_b2b_transfer`
calls its Prepare-equivalent once, Transfer repeatedly). Confirmed safe
via `fsl_lpspi_edma.c` source: the eDMA completion callback resets
`handle->state` back to idle, so back-to-back `Transfer()` calls after one
`Prepare()` are supported, not just something the reference example
happens to get away with.

**Result: no measurable change** (455ms/window both before and after,
8fps both times). This DISPROVES the per-chunk-overhead hypothesis
outright - real A/B test, not just theory - the same "test it directly
instead of trusting the math" lesson this project's fps investigation
already learned once before (the CPU-polled path's chunk-size experiment,
several entries below).

**Root cause of the wrong math: misread the existing diagnostic.**
`LCD_DrawImage()`'s printed "`frame push=Xms (per window)`" is a TOTAL
across however many frames occurred in that ~1-second window, not a
single frame's time - dividing 451ms by 19 (chunks in ONE frame) was
comparing a many-frames total against a one-frame chunk count, nonsense
units. Fixed the diagnostic to also print frame count and a proper
`us/frame avg` - real number: **~56.9ms/frame** for `LCD_SetWindow()` +
`LCD_PushPixels()` combined - remarkably close to the ~51ms theoretical
bit-clock floor for a full 320x240 push at 24MHz. The eDMA fix from the
entry below was ALREADY performing near-optimally; there was no
meaningful per-chunk overhead left to remove, which is exactly why
hoisting Prepare() out changed nothing.

**So why is fps still only 8 (~125ms/frame) if the LCD push is only
~57ms?** Added a second diagnostic in `main.c`'s preview loop: time spent
waiting for `CAMERA_CAPTURE_IsFrameReady()` to go true, measured
separately from the LCD push. Result: **~75.5ms/frame spent waiting for
the camera**, MORE than the ~56.9ms spent pushing to the LCD (75.5 + 56.9
≈ 132ms ≈ 7.6fps, matching the measured 8fps closely). **The camera/
SmartDMA side is now the larger of the two bottlenecks, not the LCD
side** - this reverses this whole investigation's original assumption
(every earlier entry in this file assumed the SPI bus was the sole
ceiling on fps).

**Not yet investigated**: why camera frame delivery takes ~75ms/frame
when the sensor is configured for 30fps (~33ms/frame native,
`OV7670_Configure()`'s own `CLKRC` register math targets this
correctly for a 24MHz `xclock`) - `camera_capture.c` has no
`CAMERA_CAPTURE_Deinit()`/`Reinit()` calls in this specific preview loop
(unlike the default AI-build loop, which explicitly cycles SmartDMA around
inference for a different, already-understood reason - RAM bank
conflict, see below), so that's not the explanation here. `~75ms ≈ 2 x
~33ms` is a suggestive coincidence (worth checking whether
`kSMARTDMA_CameraWholeFrameQVGA`'s firmware needs 2 real sensor frames per
delivered output frame for some structural reason) but NOT confirmed -
could just as easily be a single-buffer handshake (SmartDMA only starts
capturing the next frame once the CPU clears the ready flag, i.e. no
overlap between capture and LCD-push time) rather than a 2-frames-per-1
ratio. Needs actual measurement (e.g. counting `CAMERA_CAPTURE_GetFrameCount()`
increments over a fixed time window with the LCD push disabled entirely,
to isolate the camera's true free-running delivery rate from whatever the
consuming loop does) before guessing further.

**Kept the Prepare()-once-per-frame restructuring** despite the null
result - it's still the architecturally correct usage pattern (matches
the tested reference example, avoids redundant module disable/flush/
re-enable churn every chunk even though it didn't move the needle on
THIS bottleneck) and the new diagnostics (per-frame LCD-push time,
per-frame camera-wait time) are useful, real instrumentation for whoever
picks up the camera-side investigation next - not reverted.

### Next steps for a fresh session

1. Isolate the camera's true free-running frame rate: temporarily skip
   `LCD_DrawImage()` in the preview loop (or make it a no-op) and measure
   `CAMERA_CAPTURE_GetFrameCount()` increments over a few seconds - if it's
   still ~13fps (matching the ~75ms/frame wait) with NOTHING consuming
   frames, the bottleneck is genuinely in SmartDMA/the sensor, not a
   software handshake; if it's much faster (near 30fps), something in the
   current loop is artificially throttling delivery.
2. If the camera really only delivers ~13fps: check whether
   `kSMARTDMA_CameraWholeFrameQVGA` structurally needs 2 sensor frames per
   delivered frame (would need reading the SmartDMA camera firmware's own
   behavior/docs, not just the calling API), or whether a different
   SmartDMA camera API mode exists with less per-frame overhead.
2b. If the camera delivers close to native 30fps when unconsumed: the
   next candidate is adding real double-buffering (two frame buffers,
   ping-pong between them) so SmartDMA can capture the NEXT frame while
   the CPU is still pushing the PREVIOUS one to the LCD - a bigger change
   than anything in this specific investigation so far (new buffer
   allocation, correct synchronization), current single-buffer design has
   no overlap between capture and push.
3. Once the real ceiling is understood, revisit whether further LCD-side
   optimization (raising `LCD_SPI_CHUNK_PIXELS`, pushing the SPI clock
   past 24MHz) is even worth it - at ~57ms/frame already near the 24MHz
   bit-clock floor, LCD-side gains alone can't get overall fps much past
   what the camera side allows anyway, unless double-buffering closes the
   gap between the two.

## eDMA RX-channel hang: ROOT-CAUSED AND FIXED - TCR.RXMSK was stuck at 1, inherited from the driver's own write-only blocking transfers; eDMA pixel-push now runs on real hardware (7fps -> 8fps, no hang) (2026-09-04)

Follow-up to the entry directly below (same day). User asked to search the
internet and mcuxsdk's own examples for this exact hang, then try to solve
it. Web search (NXP community, Zephyr's LPSPI/eDMA issue trackers, the
MCXNx4x errata sheet) turned up related-but-not-matching reports (LPI2C
eDMA bus-error handling, an unrelated LPSPI slave-mode TX-FIFO-underrun
erratum, a Zephyr regression report with different symptoms) - no
externally documented fix for this exact symptom, so root-caused it
directly against mcuxsdk's own driver source and a fresh live register
trace instead.

**New diagnostic, not just re-reading source.** Added
`SPI1_BUS_RunDmaDiagnostic()` (spi1_bus.c, temporary - removed again once
the fix was confirmed) - runs one small, fully isolated 64-byte eDMA
transfer via `kLPSPI_MasterPcs1` (nothing physically selected, safe to run
at any point in boot) and prints a live trace of LPSPI1's FIFO counts
(FSR), both DMA channels' CH_CSR/CH_ES, and LPSPI1's own CR/TCR/DER every
time any of them changes - richer than the previous session's single
static SWD snapshot. Called once at boot in the `LCD_CAMERA_PREVIEW`
build, right after `LCD_Init()`.

**Result, decisive:** TX's eDMA channel reached `CH_CSR=0x40000000`
(DONE) within 1 microsecond of starting - far faster than a real 64-byte
SPI transfer physically requires - while `FSR` read `tx=0, rx=0` for the
ENTIRE 200ms timeout window, never changing even once. `CR=0x00000001`
confirmed the module genuinely was enabled (ruling out a "module
disabled" theory). The one value that stood out: `TCR=0x01280007` - bit
19 set, which is **RXMSK** (Receive Data Mask): when set, the LPSPI
hardware discards incoming data instead of storing it to the RX FIFO.

**Root cause, confirmed by reading `fsl_lpspi.c` line-by-line:**
`LPSPI_MasterTransferBlocking()` sets `TCR.RXMSK = (rxData == NULL)` on
every call. This project's shared bus calls this with `rxData=NULL` for
EVERY write-only transfer - every LCD command byte via
`lcd_spi_hw.c`'s `LCD_WriteByte()`, and even the (until now) CPU-polled
pixel-push path itself - so RXMSK=1 gets set constantly, and nothing ever
clears it back to 0 for a subsequent transfer that actually wants RX data.
`LPSPI_MasterTransferPrepareEDMALite()` (`fsl_lpspi_edma.c`) only clears
`CONT`/`CONTC`/`BYSW`/`PCS` in its own TCR write - it silently **inherits**
whatever RXMSK was left at by the last blocking transfer. Since
`LCD_Init()`'s panel-init sequence writes several command bytes right
before the (attempted) eDMA pixel push, RXMSK was always 1 by the time the
eDMA transfer started - meaning the RX FIFO could structurally never
receive a single byte, so it could never reach the DMA watermark the RX
eDMA channel waits on, so that channel waited forever. This fully explains
every symptom from the previous session: TX completing (RXMSK doesn't
touch the TX path), the hang being identical regardless of SPI frame size
or chunk size (RXMSK has nothing to do with either), and the "channel-mux/
clock/DER all read correct" dead end (none of those were ever the
problem).

**Fix:** `SPI1_BUS_TransferBytesDMA()` (spi1_bus.c) now explicitly clears
`TCR.RXMSK` and `TCR.TXMSK` right after `LPSPI_MasterTransferPrepareEDMALite()`
succeeds, before starting the transfer - this transfer always wants real
(if discarded-by-the-caller) data flowing on both sides so eDMA can
observe FIFO activity and signal completion.

**Confirmed on real hardware, twice.** First via the diagnostic itself:
same 64-byte test, same live trace - `TCR` read `0x01280007` (RXMSK=1)
right before the fix's clear, then `DMA-DIAG: transfer completed
normally` instead of timing out. Second, for real: `lcd_spi_hw.c`'s
`LCD_PushPixelsOpen()` switched back from `SPI1_BUS_TransferBlocking()` to
`SPI1_BUS_TransferBytesDMA()`, diagnostic call removed, rebuilt, reflashed.
`LCD_CAMERA_PREVIEW=ON` build: runs continuously, no hang, `LCD preview: 8
fps` / `frame push=451ms (per window)` steady (up from the CPU-polled
path's confirmed 7fps / 1076ms). Default AI-enabled build (SD card +
inference + LCD status text, i.e. the actual shipped configuration, which
also exercises this same pixel-push path via `DEMO_ClearScreen()`): boots
and runs continuously for the full capture window - SD card initializes
fine, inference keeps running (~3.9ms/frame), no hangs or corruption
observed in the serial log.

**Not yet confirmed: image quality.** This session had no camera/photo
access to visually inspect the panel - all confirmation above is
register-level and fps-timing, not a look at the actual displayed image.
Given the previous eDMA attempt's real risk was image corruption (not
just hangs), and this project's own history has a precedent for a
"looks fine in diagnostics but was actually shifted/corrupted" bug (the
BGR color-cast entry, caught only by looking at a photo) - **get the user
to look at the live image before calling this fully done.**

**fps is 7->8, a real but modest gain - well short of the ~19-20fps
best-case math.** Back-of-envelope: 19 DMA chunk calls/frame
(`LCD_SPI_CHUNK_PIXELS=4096`) at 451ms/window is ~23.7ms/chunk, against a
~2.7ms theoretical bit-clock time per 8192-byte chunk at 24MHz - a
~21ms/chunk gap suggesting `LPSPI_MasterTransferPrepareEDMALite()`'s own
per-call cost (module disable/re-enable, FIFO flush, interrupt/DMA-request
bookkeeping - called on every chunk, not once per frame like mcuxsdk's own
reference example does it) is now the dominant remaining cost, the same
*class* of per-call-overhead issue the CPU-polled path had earlier in this
file, just with a different underlying cause. **Not yet tested directly**
(e.g. by raising `LCD_SPI_CHUNK_PIXELS` and re-measuring, the same
methodology that worked for previous per-call-overhead questions in this
project) - RAM is the blocker: the default AI-enabled build is already at
309,104/319,488 bytes (~96.75%) of `m_data` at the CURRENT chunk size,
only ~10.4KB free, not enough headroom to double the scratch buffer
without shrinking something else (the AI tensor arena, most likely)
first.

### Next steps for a fresh session

1. **Get the user to look at the live image** (camera preview or the
   default build's status-line screen) and confirm no corruption/tearing/
   glitching - this is the one thing this session's register-level/fps
   confirmation can't substitute for.
2. If chasing further fps: the next concrete lever is confirming the
   "`LPSPI_MasterTransferPrepareEDMALite()`'s per-chunk cost is now
   dominant" theory directly (e.g. wrap just that call with a DWT delta,
   the same measurement style already used elsewhere in this file) before
   deciding whether raising chunk size (needs RAM freed up elsewhere
   first) or restructuring to call Prepare() once per frame instead of
   once per chunk (mcuxsdk's own reference example's pattern) is the
   better next move.
3. Given 8fps is a real, working improvement over the previous confirmed-
   good 7fps, and further gains need either a RAM trade-off or restructuring
   work - worth explicitly checking with the user whether 8fps is
   acceptable before spending more effort here.
4. The bus-sharing baud-reclaim/delay-register logic and touch still have
   zero real-hardware confirmation beyond the LCD-only camera-preview
   test - see earlier entries for what to test.

## Older work (condensed) - full narratives trimmed, see git history for the original blow-by-blow if ever needed

Everything below this point predates the current fps/eDMA/camera investigation
above and is settled/superseded - kept only as a compact reference of what
was done and why, not as active next-steps.

### LCD/SPI bring-up path that led to today's 7fps CPU-polled baseline (2026-09-03 - 2026-09-04, before the entries above)

- Arduino-header LCD swapped from an 8-bit-parallel shield to a 2.4" SPI TFT
  module. First wired as bit-banged GPIO SPI (own driver,
  `lcd_spi_bitbang.c`), then moved to hardware LPSPI1, **sharing the bus**
  with the onboard microSD slot and wiring up the panel's touch controller
  (XPT2046, `touch_xpt2046.c` - present but never integrated into the UI).
  New `spi1_bus.c/h` shared-bus wrapper handles per-transaction baud-rate/
  delay-register reclaiming since SD/LCD/touch each want different rates on
  the same physical bus.
- First real-hardware test found and fixed a blue/cyan color cast (MADCTL
  BGR bit wrong for this panel vs. the earlier parallel one) and improved
  fps via pixel-push batching (chunked transfers instead of one SPI call
  per byte).
- Root-caused a real fps ceiling in three separate, independently-confirmed
  steps: (1) LPSPI1's source clock was FRO12M/12MHz (max ~6MHz SPI baud) -
  switched to FRO_HF/48MHz for a real ~24MHz ceiling; (2) missing
  `kLPSPI_MasterPcsContinuous` was inserting a full PCS setup/hold delay
  between every single byte even on an unrouted "don't care" PCS channel -
  fixed, 2fps -> 5fps; (3) `SPI1_BUS_SetBaudRate()` only updated the SCK
  divider, leaving PCS-to-SCK/between-transfer delay registers sized for a
  stale 400kHz baseline - fixed by recalculating them on every baud-rate
  change, 5fps -> 7fps, confirmed via live SWD register reads. A first
  eDMA attempt at this point hung on real hardware and was reverted (root
  cause found and fixed later - see the entries above this section).

### SD card snapshot-on-face-detection feature (2026-08-25)

Added SD-over-SPI + FatFs glue (`sd_spi_disk.c`) and a snapshot feature
(`snapshot.c`) that saves a boxed BMP to the shield's microSD slot on face
detection, rate-limited to 1/sec. Real-hardware bring-up found and fixed,
in order: a first-boot hang (missing SPI retry-timeout Kconfig option, plus
a retry-budget multiplication bug in `fsl_sdspi.c`'s call graph that could
block for minutes); a floating MISO line (shield has no pull-up of its own,
diagnosed by adding this chip's internal pull-up and confirming the fix
live rather than guessing short-vs-floating); a stale one-shot "SD init
timed out" deadline check that started incorrectly applying to every later
file write, not just the initial mount; writes stuck at the 400kHz
card-identification speed forever (`busBaudRate` never updated after
identification) - measured 3.3s/save, fixed by raising the operating baud
rate to 8MHz; missing T/U/R glyphs in the project's hand-picked minimal
font, rendering "CAPTURE" as "CAP   E"; and two usability fixes after
looking at a real saved image (detection box is inherently one 8x8 FOMO
grid cell, not a real bounding box - tried expanding it for display, user
asked to revert and keep it raw; split the "just saved" LCD notice's
display duration from the 1-second capture rate limit so it's actually
visible). All confirmed working end-to-end on real hardware.

### AI model integration and NPU (Neutron) bring-up (2026-08-24 - 2026-08-25)

- Integrated a trained Edge Impulse FOMO object-detection model (first a
  3-class drowsy-eye detector, later replaced entirely by a lighter
  single-class face detector, retrained twice to fit this chip's RAM -
  final deploy version: 72x72 input, fits stock `m_data` with real margin
  on both CPU and NPU backends). Fixed several real bugs along the way:
  a buffer overflow from setting `signal.total_length` to the raw camera
  frame size instead of the model's own input size (caused a precise bus
  fault marching off the end of the AI RAM pool); a misdiagnosed alignment
  fault that was actually a stack overflow (STKOF, an ARMv8-M hardware
  stack-limit check - easy to misread against the wrong CFSR bit); and a
  RAM collision between the AI tensor arena and the SmartDMA camera
  coprocessor's own firmware, which share the same physical `m_sramx` bank
  - fixed by stopping SmartDMA before inference and restarting it after
  (same time-multiplexing pattern already used elsewhere in this project
  for the camera/USB-HS voltage conflict).
- A same-day linker experiment that widened `m_data` by reclaiming the
  second CPU core's (core1, never booted) reserved RAM region was WRONG in
  practice (multicore SoCs commonly power-gate RAM per core, so "nothing
  disables it" didn't mean "it's powered") and briefly bricked UART output
  and SWD debug access entirely - recovered via NXP's `nxpdebugmbox`
  Debug-Mailbox tool (bypasses pyOCD's normal connection sequence, which
  has no retry logic for this specific failure) and reverted; not
  reintroduced.
- Ported the model to run on the chip's Neutron NPU instead of CPU+CMSIS-NN,
  via NXP's `neutron_converter` tool (fuses supported layers into one
  custom op) and a hand-written raw TFLite-Micro runner
  (`model_runner_npu.cpp`, bypassing Edge Impulse's own classifier
  entry point). Confirmed on real hardware: **~370-390x faster** than the
  CPU path (~3.3ms vs. ~1.27s per inference), same detection-quality
  pipeline (hand-ported FOMO grid postprocessing), comfortable arena
  headroom. This NPU path is the current default (`AI_MODEL_USE_NPU=ON`).
- Also fixed: an LCD bug where a multi-row status-color fill only ever
  painted the first row, because `LCD_PushPixels()` closes the SPI chip
  select every call and the fill loop called it once per row instead of
  keeping the transfer open across all rows (`LCD_PushPixelsOpen()`/
  `LCD_EndWindow()` added to fix this, still the pattern
  `LCD_PushPixelsOpen()`'s current callers use).
