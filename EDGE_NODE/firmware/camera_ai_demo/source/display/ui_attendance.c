/*
 * ui_attendance.c - Lock UI for Attendance Mode (UI-7..UI-15)
 * Portrait Orientation (240x320) with Sunset Mountain Wallpaper & Sans-Serif Typography
 */
#include "ui_attendance.h"
#include "app.h"
#include "font_sans.h"
#include "lcd_display.h"
#include "wallpaper_240x320.h"
#include <stdio.h>
#include <string.h>

#define CIRCLE_DIAMETER   160U
#define CIRCLE_RADIUS     80
#define CIRCLE_X0         40U
#define CIRCLE_Y0         30U
#define CIRCLE_R_IN_SQ    (70 * 70)  /* 4900: inner face boundary (140px diameter) */
#define CIRCLE_R_OUT_SQ   (78 * 78)  /* 6084: outer ring boundary (8px ring thickness) */

static ui_attendance_state_t s_uiState = UI_ATTENDANCE_STATE_IDLE;
static uint32_t s_lastRenderedSec = 0xFFFFFFFFU;
static bool     s_lastOnline      = false;
static bool     s_lastSynced      = false;

/* Cached face crop parameters */
static const uint16_t *s_cachedFrame    = NULL;
static uint16_t        s_faceCenterCamX = 160U;
static uint16_t        s_faceCenterCamY = 120U;
static uint16_t        s_faceScaleFp8   = 360U;
static uint8_t         s_animStep       = 0U;
static bool            s_faceGranted    = false;

static void ClearScreen(uint16_t color)
{
    uint16_t line[DEMO_PANEL_WIDTH];
    for (uint16_t i = 0U; i < DEMO_PANEL_WIDTH; i++)
    {
        line[i] = color;
    }
    LCD_SetWindow(0U, 0U, DEMO_PANEL_WIDTH - 1U, DEMO_PANEL_HEIGHT - 1U);
    for (uint16_t row = 0U; row < DEMO_PANEL_HEIGHT; row++)
    {
        LCD_PushPixelsOpen(line, DEMO_PANEL_WIDTH);
    }
    LCD_EndWindow();
}

static void ClearRegion(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint16_t width = (uint16_t)(x1 - x0 + 1U);
    uint16_t line[DEMO_PANEL_WIDTH];
    for (uint16_t i = 0U; i < width; i++)
    {
        line[i] = color;
    }
    LCD_SetWindow(x0, y0, x1, y1);
    for (uint16_t row = y0; row <= y1; row++)
    {
        LCD_PushPixelsOpen(line, width);
    }
    LCD_EndWindow();
}

/* Draws a Sans-serif text string (16px tall) with wallpaper background sampling & drop shadow */
static void DrawTextOnWallpaper(uint16_t x, uint16_t y, const char *str, uint16_t fgColor)
{
    uint16_t totalW = FONT_GetTextStringWidth(str);
    if (totalW == 0U)
    {
        return;
    }
    uint16_t totalH = FONT_TEXT_HEIGHT;

    if (x + totalW > DEMO_PANEL_WIDTH)
    {
        totalW = (uint16_t)(DEMO_PANEL_WIDTH - x);
    }
    if (y + totalH > DEMO_PANEL_HEIGHT)
    {
        totalH = (uint16_t)(DEMO_PANEL_HEIGHT - y);
    }

    /* Build glyph cache for this string */
    uint32_t len = strlen(str);
    const font_text_glyph_t *glyphs[40];
    uint16_t glyphX[40];
    if (len > 39U)
    {
        len = 39U;
    }

    uint16_t curX = 0U;
    for (uint32_t i = 0U; i < len; i++)
    {
        glyphs[i] = FONT_GetTextGlyph(str[i]);
        glyphX[i] = curX;
        curX = (uint16_t)(curX + glyphs[i]->adv);
    }

    static uint16_t lineBuf[DEMO_PANEL_WIDTH];
    LCD_SetWindow(x, y, (uint16_t)(x + totalW - 1U), (uint16_t)(y + totalH - 1U));

    for (uint16_t row = 0U; row < totalH; row++)
    {
        uint16_t py = (uint16_t)(y + row);
        uint32_t wallRowOffset = (uint32_t)py * DEMO_PANEL_WIDTH;

        for (uint16_t col = 0U; col < totalW; col++)
        {
            uint16_t px = (uint16_t)(x + col);
            bool pixelOn = false;
            bool shadowOn = false;

            /* Check glyphs overlapping this col */
            for (uint32_t i = 0U; i < len; i++)
            {
                if (col >= glyphX[i] && col < glyphX[i] + glyphs[i]->adv)
                {
                    uint16_t gx = (uint16_t)(col - glyphX[i]);
                    if (gx < 16U)
                    {
                        pixelOn = ((glyphs[i]->rows[row] >> (15U - gx)) & 1U) != 0U;
                    }
                    if (!pixelOn && row >= 1U && gx >= 1U)
                    {
                        shadowOn = ((glyphs[i]->rows[row - 1U] >> (15U - (gx - 1U))) & 1U) != 0U;
                    }
                    break;
                }
            }

            if (pixelOn)
            {
                lineBuf[col] = fgColor;
            }
            else if (shadowOn)
            {
                lineBuf[col] = 0x0841U; /* Dark shadow outline for readability on wallpaper */
            }
            else
            {
                lineBuf[col] = WALLPAPER_240X320[wallRowOffset + px];
            }
        }
        LCD_PushPixelsOpen(lineBuf, totalW);
    }
    LCD_EndWindow();
}

static void DrawCenteredTextOnWallpaper(uint16_t y, const char *str, uint16_t fgColor)
{
    uint16_t totalW = FONT_GetTextStringWidth(str);
    uint16_t x = (totalW < DEMO_PANEL_WIDTH) ? (uint16_t)((DEMO_PANEL_WIDTH - totalW) / 2U) : 0U;
    DrawTextOnWallpaper(x, y, str, fgColor);
}

/* Draws the Big Digital Clock (32px tall Sans-serif) with wallpaper sampling & shadow */
static void DrawClockOnWallpaper(uint16_t y, const char *str, uint16_t fgColor)
{
    uint16_t totalW = FONT_GetClockStringWidth(str);
    if (totalW == 0U)
    {
        return;
    }
    uint16_t totalH = FONT_CLOCK_HEIGHT;
    uint16_t x = (totalW < DEMO_PANEL_WIDTH) ? (uint16_t)((DEMO_PANEL_WIDTH - totalW) / 2U) : 0U;

    uint32_t len = strlen(str);
    const font_clock_glyph_t *glyphs[16];
    uint16_t glyphX[16];
    if (len > 15U)
    {
        len = 15U;
    }

    uint16_t curX = 0U;
    for (uint32_t i = 0U; i < len; i++)
    {
        glyphs[i] = FONT_GetClockGlyph(str[i]);
        glyphX[i] = curX;
        curX = (uint16_t)(curX + glyphs[i]->adv);
    }

    static uint16_t lineBuf[DEMO_PANEL_WIDTH];
    LCD_SetWindow(x, y, (uint16_t)(x + totalW - 1U), (uint16_t)(y + totalH - 1U));

    for (uint16_t row = 0U; row < totalH; row++)
    {
        uint16_t py = (uint16_t)(y + row);
        uint32_t wallRowOffset = (uint32_t)py * DEMO_PANEL_WIDTH;

        for (uint16_t col = 0U; col < totalW; col++)
        {
            uint16_t px = (uint16_t)(x + col);
            bool pixelOn = false;
            bool shadowOn = false;

            for (uint32_t i = 0U; i < len; i++)
            {
                if (col >= glyphX[i] && col < glyphX[i] + glyphs[i]->adv)
                {
                    uint16_t gx = (uint16_t)(col - glyphX[i]);
                    if (gx < 24U)
                    {
                        pixelOn = ((glyphs[i]->rows[row] >> (23U - gx)) & 1U) != 0U;
                    }
                    if (!pixelOn && row >= 1U && gx >= 1U)
                    {
                        shadowOn = ((glyphs[i]->rows[row - 1U] >> (23U - (gx - 1U))) & 1U) != 0U;
                    }
                    break;
                }
            }

            if (pixelOn)
            {
                lineBuf[col] = fgColor;
            }
            else if (shadowOn)
            {
                lineBuf[col] = 0x0841U; /* Sharp dark shadow */
            }
            else
            {
                lineBuf[col] = WALLPAPER_240X320[wallRowOffset + px];
            }
        }
        LCD_PushPixelsOpen(lineBuf, totalW);
    }
    LCD_EndWindow();
}

static void DrawCenteredTextSolid(uint16_t y, const char *str, uint16_t fgColor, uint16_t bgColor)
{
    uint16_t totalW = FONT_GetTextStringWidth(str);
    if (totalW == 0U)
    {
        return;
    }
    uint16_t totalH = FONT_TEXT_HEIGHT;
    uint16_t x = (totalW < DEMO_PANEL_WIDTH) ? (uint16_t)((DEMO_PANEL_WIDTH - totalW) / 2U) : 0U;

    uint32_t len = strlen(str);
    const font_text_glyph_t *glyphs[40];
    uint16_t glyphX[40];
    if (len > 39U)
    {
        len = 39U;
    }

    uint16_t curX = 0U;
    for (uint32_t i = 0U; i < len; i++)
    {
        glyphs[i] = FONT_GetTextGlyph(str[i]);
        glyphX[i] = curX;
        curX = (uint16_t)(curX + glyphs[i]->adv);
    }

    static uint16_t lineBuf[DEMO_PANEL_WIDTH];
    LCD_SetWindow(x, y, (uint16_t)(x + totalW - 1U), (uint16_t)(y + totalH - 1U));

    for (uint16_t row = 0U; row < totalH; row++)
    {
        for (uint16_t col = 0U; col < totalW; col++)
        {
            bool pixelOn = false;
            for (uint32_t i = 0U; i < len; i++)
            {
                if (col >= glyphX[i] && col < glyphX[i] + glyphs[i]->adv)
                {
                    uint16_t gx = (uint16_t)(col - glyphX[i]);
                    if (gx < 16U)
                    {
                        pixelOn = ((glyphs[i]->rows[row] >> (15U - gx)) & 1U) != 0U;
                    }
                    break;
                }
            }
            lineBuf[col] = pixelOn ? fgColor : bgColor;
        }
        LCD_PushPixelsOpen(lineBuf, totalW);
    }
    LCD_EndWindow();
}

static void UnixToDate(uint32_t unixTime, uint16_t *year, uint8_t *month, uint8_t *day,
                       uint8_t *hour, uint8_t *min, uint8_t *sec, uint8_t *wday)
{
    /* Convert to UTC+7 (Vietnam standard time) */
    unixTime += 7U * 3600U;
    *sec = (uint8_t)(unixTime % 60U);
    unixTime /= 60U;
    *min = (uint8_t)(unixTime % 60U);
    unixTime /= 60U;
    *hour = (uint8_t)(unixTime % 24U);
    uint32_t days = unixTime / 24U;
    *wday = (uint8_t)((days + 4U) % 7U); /* 1970-01-01 was Thursday = 4 */

    uint32_t y = 1970U;
    while (1)
    {
        uint32_t daysInYear = ((y % 4U == 0U && (y % 100U != 0U || y % 400U == 0U))) ? 366U : 365U;
        if (days < daysInYear)
        {
            break;
        }
        days -= daysInYear;
        y++;
    }
    *year = (uint16_t)y;

    static const uint8_t daysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4U == 0U && (y % 100U != 0U || y % 400U == 0U));
    uint8_t m = 0U;
    for (m = 0U; m < 12U; m++)
    {
        uint8_t dim = daysInMonth[m];
        if (m == 1U && leap)
        {
            dim = 29U;
        }
        if (days < dim)
        {
            break;
        }
        days -= dim;
    }
    *month = (uint8_t)(m + 1U);
    *day   = (uint8_t)(days + 1U);
}

/* Fast 8-octant compass heading (0..7 clockwise starting from North) */
static inline uint8_t GetOctant(int16_t dx, int16_t dy)
{
    int16_t ax = (dx >= 0) ? dx : -dx;
    int16_t ay = (dy >= 0) ? dy : -dy;

    if (dx >= 0 && dy < 0)
    {
        return (ay >= ax) ? 0U : 1U;
    }
    if (dx >= 0 && dy >= 0)
    {
        return (ax >= ay) ? 2U : 3U;
    }
    if (dx < 0 && dy >= 0)
    {
        return (ay >= ax) ? 4U : 5U;
    }
    return (ax >= ay) ? 6U : 7U;
}

static void RenderCircularPortrait(void)
{
    if (s_cachedFrame == NULL)
    {
        return;
    }

    uint16_t lineBuf[CIRCLE_DIAMETER];
    LCD_SetWindow(CIRCLE_X0, CIRCLE_Y0, (uint16_t)(CIRCLE_X0 + CIRCLE_DIAMETER - 1U),
                  (uint16_t)(CIRCLE_Y0 + CIRCLE_DIAMETER - 1U));

    for (uint16_t row = 0U; row < CIRCLE_DIAMETER; row++)
    {
        int16_t dy = (int16_t)row - (int16_t)CIRCLE_RADIUS;
        int32_t dy2 = (int32_t)dy * (int32_t)dy;

        /* Precompute dy offset in camera frame:
         * Top of circle (dy < 0) -> Forehead -> Larger camX
         * Bottom of circle (dy > 0) -> Chin -> Smaller camX
         */
        int32_t dCamX = -((int32_t)dy * (int32_t)s_faceScaleFp8) >> 8U;
        int32_t baseCamX = (int32_t)s_faceCenterCamX + dCamX;

        for (uint16_t col = 0U; col < CIRCLE_DIAMETER; col++)
        {
            int16_t dx = (int16_t)col - (int16_t)CIRCLE_RADIUS;
            int32_t distSq = (int32_t)dx * (int32_t)dx + dy2;

            if (distSq < CIRCLE_R_IN_SQ)
            {
                /* Left of circle (dx < 0) -> Smaller camY
                 * Right of circle (dx > 0) -> Larger camY
                 */
                int32_t dCamY = ((int32_t)dx * (int32_t)s_faceScaleFp8) >> 8U;
                int32_t camX = baseCamX;
                int32_t camY = (int32_t)s_faceCenterCamY + dCamY;

                /* Clamp to camera frame (320x240) */
                if (camX < 0) camX = 0;
                if (camX > 319) camX = 319;
                if (camY < 0) camY = 0;
                if (camY > 239) camY = 239;

                lineBuf[col] = s_cachedFrame[(uint32_t)camY * 320U + (uint32_t)camX];
            }
            else if (distSq <= CIRCLE_R_OUT_SQ)
            {
                /* Ring pixel */
                if (s_uiState == UI_ATTENDANCE_STATE_RESULT_FACE)
                {
                    lineBuf[col] = s_faceGranted ? UI_COLOR_GREEN : UI_COLOR_RED;
                }
                else
                {
                    /* Animated spinner */
                    uint8_t oct = GetOctant(dx, dy);
                    if (oct == s_animStep)
                    {
                        lineBuf[col] = UI_COLOR_CYAN;
                    }
                    else if (oct == ((s_animStep + 7U) & 7U))
                    {
                        lineBuf[col] = 0x03FFU; /* Medium cyan/blue trail */
                    }
                    else if (oct == ((s_animStep + 6U) & 7U))
                    {
                        lineBuf[col] = 0x01FFU; /* Faint trail */
                    }
                    else
                    {
                        lineBuf[col] = UI_COLOR_DARK_GRAY;
                    }
                }
            }
            else
            {
                lineBuf[col] = UI_COLOR_BG;
            }
        }
        LCD_PushPixelsOpen(lineBuf, CIRCLE_DIAMETER);
    }
    LCD_EndWindow();
}

void UI_ATTENDANCE_Init(void)
{
    s_uiState = UI_ATTENDANCE_STATE_IDLE;
    s_lastRenderedSec = 0xFFFFFFFFU;
    s_lastOnline      = false;
    s_lastSynced      = false;
    s_cachedFrame     = NULL;

    /* Paint full 240x320 sunset mountain wallpaper */
    LCD_DrawImage(0U, 0U, DEMO_PANEL_WIDTH, DEMO_PANEL_HEIGHT, WALLPAPER_240X320);
}

void UI_ATTENDANCE_UpdateClock(uint32_t unixTime, bool online, bool forceRedraw)
{
    s_uiState = UI_ATTENDANCE_STATE_IDLE;

    bool timeSynced = (unixTime > 0U);
    uint32_t currentSec = timeSynced ? (unixTime % 60U) : 0xFFFFFFFFU;

    if (!forceRedraw && (currentSec == s_lastRenderedSec) && (online == s_lastOnline) && (timeSynced == s_lastSynced))
    {
        return; /* Nothing changed, skip redraw entirely (zero bus contention) */
    }

    if (forceRedraw)
    {
        /* Redraw full wallpaper background */
        LCD_DrawImage(0U, 0U, DEMO_PANEL_WIDTH, DEMO_PANEL_HEIGHT, WALLPAPER_240X320);

        /* Permanent Header & Footer elements with shadow */
        DrawTextOnWallpaper(138U, 14U, "LOCK ACTIVE", UI_COLOR_WHITE);
        DrawCenteredTextOnWallpaper(240U, "Scan Face", UI_COLOR_CYAN);
        DrawCenteredTextOnWallpaper(265U, "or Tap RFID Card", UI_COLOR_WHITE);
        DrawCenteredTextOnWallpaper(298U, "MCXN947 Smart Lock", UI_COLOR_GRAY);
    }

    /* Update Top Bar Online / Offline Status */
    if (forceRedraw || (online != s_lastOnline))
    {
        if (online)
        {
            DrawTextOnWallpaper(10U, 14U, "[WiFi ONLINE] ", UI_COLOR_GREEN);
        }
        else
        {
            DrawTextOnWallpaper(10U, 14U, "[WiFi OFFLINE]", UI_COLOR_YELLOW);
        }
        s_lastOnline = online;
    }

    /* Digital Clock and Date */
    char timeStr[16];
    char dateStr[24];

    if (!timeSynced)
    {
        (void)snprintf(timeStr, sizeof(timeStr), "--:--:--");
        (void)snprintf(dateStr, sizeof(dateStr), "Syncing time...");
    }
    else
    {
        uint16_t year;
        uint8_t month, day, hour, min, sec, wday;
        UnixToDate(unixTime, &year, &month, &day, &hour, &min, &sec, &wday);

        static const char *const wdayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        (void)snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u", hour, min, sec);
        (void)snprintf(dateStr, sizeof(dateStr), "%04u-%02u-%02u  %s", year, month, day, wdayNames[wday % 7U]);
    }

    /* Big Sans-Serif Clock (32px tall, centered) */
    DrawClockOnWallpaper(80U, timeStr, UI_COLOR_WHITE);

    /* Clean Sans-Serif Date (16px tall, centered) */
    DrawCenteredTextOnWallpaper(125U, dateStr, UI_COLOR_WHITE);

    s_lastRenderedSec = currentSec;
    s_lastSynced      = timeSynced;
}

void UI_ATTENDANCE_ShowVerifying(const uint16_t *frame, uint16_t faceCenterX, uint16_t faceCenterY,
                                uint16_t faceBoxSize, uint8_t animStep)
{
    s_uiState = UI_ATTENDANCE_STATE_VERIFYING;
    s_cachedFrame = frame;
    s_animStep = animStep & 7U;

    s_faceCenterCamX = faceCenterX;
    s_faceCenterCamY = faceCenterY;

    /* Scale so the entire face with generous margin fits in the 96px inner circle:
     * targetCoverage is ~140% of the face bounding box so forehead, hair,
     * ears and chin are completely visible. */
    uint32_t targetCoverage = (uint32_t)faceBoxSize * 140U / 100U;
    if (targetCoverage < 130U)
    {
        targetCoverage = 130U;
    }
    if (targetCoverage > 220U)
    {
        targetCoverage = 220U;
    }
    /* Inner circle diameter is 140px (2 * 70) */
    s_faceScaleFp8 = (uint16_t)((targetCoverage << 8U) / 140U);

    ClearScreen(UI_COLOR_BG);
    RenderCircularPortrait();

    DrawCenteredTextSolid(205U, "VERIFYING...", UI_COLOR_YELLOW, UI_COLOR_BG);
    DrawCenteredTextSolid(235U, "Please hold still", UI_COLOR_WHITE, UI_COLOR_BG);
}

void UI_ATTENDANCE_SpinRing(void)
{
    if (s_uiState != UI_ATTENDANCE_STATE_VERIFYING)
    {
        return;
    }
    s_animStep = (s_animStep + 1U) & 7U;
    RenderCircularPortrait();
}

void UI_ATTENDANCE_ShowFaceResult(bool granted, const char *name)
{
    s_uiState = UI_ATTENDANCE_STATE_RESULT_FACE;
    s_faceGranted = granted;

    RenderCircularPortrait();

    ClearRegion(0U, 195U, DEMO_PANEL_WIDTH - 1U, DEMO_PANEL_HEIGHT - 1U, UI_COLOR_BG);

    if (granted)
    {
        DrawCenteredTextSolid(205U, "ACCESS GRANTED", UI_COLOR_GREEN, UI_COLOR_BG);
        char welcome[40];
        if (name != NULL && name[0] != '\0')
        {
            (void)snprintf(welcome, sizeof(welcome), "Welcome, %s", name);
        }
        else
        {
            (void)snprintf(welcome, sizeof(welcome), "Welcome");
        }
        DrawCenteredTextSolid(235U, welcome, UI_COLOR_WHITE, UI_COLOR_BG);
    }
    else
    {
        DrawCenteredTextSolid(205U, "ACCESS DENIED", UI_COLOR_RED, UI_COLOR_BG);
        DrawCenteredTextSolid(235U, "Not recognized", UI_COLOR_WHITE, UI_COLOR_BG);
        DrawCenteredTextSolid(260U, "Please use card", UI_COLOR_YELLOW, UI_COLOR_BG);
    }
}

void UI_ATTENDANCE_ShowCardResult(bool granted, const char *name)
{
    s_uiState = UI_ATTENDANCE_STATE_RESULT_CARD;
    ClearScreen(UI_COLOR_BG);

    if (granted)
    {
        DrawCenteredTextSolid(40U, "[ CARD ACCESS ]", UI_COLOR_GREEN, UI_COLOR_BG);
        DrawCenteredTextSolid(100U, "ACCESS GRANTED", UI_COLOR_GREEN, UI_COLOR_BG);
        char welcome[40];
        if (name != NULL && name[0] != '\0')
        {
            (void)snprintf(welcome, sizeof(welcome), "Welcome, %s", name);
        }
        else
        {
            (void)snprintf(welcome, sizeof(welcome), "Welcome");
        }
        DrawCenteredTextSolid(145U, welcome, UI_COLOR_WHITE, UI_COLOR_BG);
        DrawCenteredTextSolid(195U, "Door Unlocked", UI_COLOR_CYAN, UI_COLOR_BG);
    }
    else
    {
        DrawCenteredTextSolid(40U, "[ CARD ACCESS ]", UI_COLOR_RED, UI_COLOR_BG);
        DrawCenteredTextSolid(100U, "ACCESS DENIED", UI_COLOR_RED, UI_COLOR_BG);
        DrawCenteredTextSolid(145U, "Card Not Registered", UI_COLOR_WHITE, UI_COLOR_BG);
        DrawCenteredTextSolid(195U, "Please Contact Admin", UI_COLOR_GRAY, UI_COLOR_BG);
    }
}

void UI_ATTENDANCE_ShowOfflineNotice(void)
{
    s_uiState = UI_ATTENDANCE_STATE_OFFLINE_NOTICE;
    ClearScreen(UI_COLOR_BG);

    DrawCenteredTextSolid(40U, "[ OFFLINE MODE ]", UI_COLOR_YELLOW, UI_COLOR_BG);
    DrawCenteredTextSolid(100U, "FACE UNAVAILABLE", UI_COLOR_YELLOW, UI_COLOR_BG);
    DrawCenteredTextSolid(145U, "Server connection offline", UI_COLOR_WHITE, UI_COLOR_BG);
    DrawCenteredTextSolid(185U, "Please use your RFID card", UI_COLOR_CYAN, UI_COLOR_BG);
}

ui_attendance_state_t UI_ATTENDANCE_GetState(void)
{
    return s_uiState;
}
