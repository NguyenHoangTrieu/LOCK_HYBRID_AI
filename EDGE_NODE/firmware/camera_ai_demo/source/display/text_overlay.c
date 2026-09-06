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

/* Widest string this project actually draws is 13 chars (e.g.
 * "CLOSED EYE: 1") at scale=3 -> 13 * (5+1)*3 = 234px. Rounded up with
 * margin; TEXT_DrawString() is only ever called with main.c's own fixed
 * status strings, not arbitrary/unbounded user input. */
#define TEXT_MAX_LINE_PIXELS 256U

static const font5x7_glyph_t *TEXT_FindGlyph(char c) {
  for (uint32_t i = 0U; i < FONT5X7_GLYPH_COUNT; i++) {
    if (FONT5X7[i].ch == c) {
      return &FONT5X7[i];
    }
  }
  return &FONT5X7[0]; /* space - fallback for any char outside this project's small glyph set */
}

void TEXT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fgColor, uint16_t bgColor, uint8_t scale) {
  static uint16_t s_rowBuf[TEXT_MAX_LINE_PIXELS];
  uint32_t len = strlen(str);
  uint16_t charCellW = (uint16_t)((FONT5X7_WIDTH + 1U) * scale); /* +1 glyph column of background as inter-char gap */
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
