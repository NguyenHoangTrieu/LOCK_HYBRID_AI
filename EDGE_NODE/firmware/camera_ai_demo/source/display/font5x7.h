/*
 * font5x7.h - hand-designed 5x7 bitmap font.
 *
 * Only covers the characters the status display in main.c actually uses
 * (F O P E N Y A W I G C L S D T U R 0 1 : space) - not a full ASCII
 * table. Each glyph is 5 columns (left to right); each column byte's
 * bit0..bit6 is that column's pixels top to bottom (bit7 unused).
 *
 * CONFIRMED on real hardware (2026-08-25): adding the "CAPTURE" status
 * line (main.c) without adding T/U/R here first rendered as "CAP   E" -
 * missing letters show up as blank glyph-width gaps, not an error/fault,
 * easy to miss in a quick look at the LCD. Check this table first if a
 * future status label renders with unexplained gaps.
 */
#ifndef _FONT5X7_H_
#define _FONT5X7_H_

#include <stdint.h>

#define FONT5X7_WIDTH 5U
#define FONT5X7_HEIGHT 7U

typedef struct { char ch; uint8_t cols[5]; } font5x7_glyph_t;

static const font5x7_glyph_t FONT5X7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, /* space */
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, /* O */
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}}, /* P */
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, /* E */
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}}, /* F - added for "FACE" status label */
    {'N', {0x7F, 0x02, 0x0C, 0x10, 0x7F}}, /* N */
    {'Y', {0x03, 0x04, 0x78, 0x04, 0x03}}, /* Y */
    {'A', {0x7C, 0x12, 0x11, 0x12, 0x7C}}, /* A */
    {'W', {0x7F, 0x20, 0x18, 0x20, 0x7F}}, /* W */
    {'I', {0x41, 0x41, 0x7F, 0x41, 0x41}}, /* I */
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x38}}, /* G */
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, /* C */
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, /* L */
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, /* S */
    {'D', {0x7F, 0x41, 0x41, 0x41, 0x3E}}, /* D */
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}}, /* T - added for "CAPTURE" status label */
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, /* U - added for "CAPTURE" status label */
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, /* R - added for "CAPTURE" status label */
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, /* 0 */
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}}, /* 1 */
    {':', {0x00, 0x00, 0x36, 0x00, 0x00}}, /* : */
};

#define FONT5X7_GLYPH_COUNT (sizeof(FONT5X7) / sizeof(FONT5X7[0]))

#endif /* _FONT5X7_H_ */
