/*
 * font_sans.h - High-quality Sans-serif font system (DejaVu Sans Bold)
 *
 * Provides:
 *  - Clock font (32px height, tabular numbers for digital lock display)
 *  - Text font (16px height, full ASCII 32..126)
 */
#ifndef _FONT_SANS_H_
#define _FONT_SANS_H_

#include <stdint.h>
#include <stdbool.h>

#define FONT_CLOCK_HEIGHT 32U
#define FONT_TEXT_HEIGHT  16U

typedef struct {
    char     ch;
    uint8_t  adv;
    uint32_t rows[FONT_CLOCK_HEIGHT];
} font_clock_glyph_t;

typedef struct {
    char     ch;
    uint8_t  adv;
    uint16_t rows[FONT_TEXT_HEIGHT];
} font_text_glyph_t;

const font_clock_glyph_t *FONT_GetClockGlyph(char c);
const font_text_glyph_t  *FONT_GetTextGlyph(char c);

uint16_t FONT_GetClockStringWidth(const char *str);
uint16_t FONT_GetTextStringWidth(const char *str);

#endif /* _FONT_SANS_H_ */
