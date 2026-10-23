/*
 * text_overlay.c - see text_overlay.h.
 *
 * Renders one text row at a time into a small static line buffer and
 * streams it straight to the LCD via LCD_SetWindow()/PushPixelsOpen(),
 * the same pattern the old solid-color status fill used - no full-screen
 * framebuffer needed.
 */
#include "text_overlay.h"
#include "font5x7.h"
#include "lcd_display.h"
#include <stdbool.h>
#include <string.h>

/* Was "widest string this project actually draws is 13 chars... rounded
 * up with margin" - that assumption was WRONG in practice: real hardware
 * HardFaulted the first time main_edge_node.c's DEMO_UpdateStatusLine()
 * called this with its 40-char padded status line at scale=2 (480px
 * needed, only 256 available) - s_rowBuf's overrun corrupted whatever
 * static object the linker happened to place after it, which surfaced
 * as an unrelated-looking crash deep inside tflite::MicroInterpreter on
 * the very next inference call. Root-caused via bisection (removing
 * RFID/actuator/state-machine code one at a time changed nothing; the
 * crash tracked exactly to the first real TEXT_DrawString() call this
 * project has ever made on hardware - main.c's own would-be caller is
 * `#if 0`'d out, so this path was never actually exercised before
 * EDGE_NODE_BUILD). Fixed at the root (clamp in TEXT_DrawString() below)
 * rather than by raising this number and hoping nothing calls it with
 * something longer later - this buffer takes arbitrary caller strings
 * now (state_machine.c's status text, up to 40 chars), not fixed,
 * pre-known-length labels. */
#define TEXT_MAX_LINE_PIXELS 320U

static const font5x7_glyph_t *TEXT_FindGlyph(char c) {
  if ((uint8_t)c >= 32U && (uint8_t)c <= 126U) {
    return &FONT5X7[(uint8_t)c - 32U];
  }
  return &FONT5X7[0]; /* space - fallback for any char outside printable ASCII */
}

void TEXT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fgColor, uint16_t bgColor, uint8_t scale) {
  static uint16_t s_rowBuf[TEXT_MAX_LINE_PIXELS];
  if (scale == 0U) {
    scale = 1U; /* a 0 scale would make charCellW 0 below (div-by-zero in the clamp) and draws nothing meaningful anyway */
  }
  uint32_t len = strlen(str);
  uint16_t charCellW = (uint16_t)((FONT5X7_WIDTH + 1U) * scale); /* +1 glyph column of background as inter-char gap */
  /* Clamp to what s_rowBuf can actually hold - see TEXT_MAX_LINE_PIXELS's
   * header comment. A too-long string is truncated (safe, just loses
   * its tail on screen), never overflows the buffer regardless of what
   * any caller passes. */
  uint32_t maxChars = TEXT_MAX_LINE_PIXELS / charCellW;
  if (len > maxChars) {
    len = maxChars;
  }
  uint16_t totalW = (uint16_t)(len * charCellW);
  uint16_t totalH = (uint16_t)(FONT5X7_HEIGHT * scale);

  LCD_SetWindow(x, y, (uint16_t)(x + totalW - 1U), (uint16_t)(y + totalH - 1U));
  for (uint16_t row = 0U; row < totalH; row++) {
    uint16_t fontRow = (uint16_t)(row / scale);
    for (uint32_t ci = 0U; ci < len; ci++) {
      const font5x7_glyph_t *glyph = TEXT_FindGlyph(str[ci]);
      uint16_t cellX = (uint16_t)(ci * charCellW);

      for (uint16_t fc = 0U; fc < FONT5X7_WIDTH; fc++) {
        bool pixelOn = ((glyph->cols[fc] >> fontRow) & 1U) != 0U;
        uint16_t color = pixelOn ? fgColor : bgColor;
        uint16_t px = (uint16_t)(cellX + fc * scale);
        for (uint16_t sx = 0U; sx < scale; sx++) {
          s_rowBuf[px + sx] = color;
        }
      }
      /* inter-char gap column, background color */
      uint16_t gapX = (uint16_t)(cellX + FONT5X7_WIDTH * scale);
      for (uint16_t sx = 0U; sx < scale; sx++) {
        s_rowBuf[gapX + sx] = bgColor;
      }
    }
    LCD_PushPixelsOpen(s_rowBuf, totalW);
  }
  LCD_EndWindow();
}
