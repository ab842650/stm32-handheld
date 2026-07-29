#include "ui.h"
#include "font.h"
#include <string.h>

void UI_DrawCentered(uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t w  = (uint16_t)strlen(s) * FONT_WIDTH;
    uint16_t tx = (ILI9341_WIDTH - w) / 2;
    ILI9341_DrawString(tx, y, s, fg, bg);
}

void UI_DrawFrame(const char *title, const char *left, const char *right)
{
    ILI9341_FillScreen(ILI9341_BLACK);

    /* 標題列 */
    ILI9341_FillRect(0, 0, ILI9341_WIDTH, UI_TITLE_H, ILI9341_NAVY);
    UI_DrawCentered((UI_TITLE_H - FONT_HEIGHT) / 2, title,
                    ILI9341_WHITE, ILI9341_NAVY);

    /* 軟鍵列 */
    ILI9341_FillRect(0, UI_SOFT_Y, ILI9341_WIDTH, UI_SOFT_H, ILI9341_NAVY);
    uint16_t sy = UI_SOFT_Y + (UI_SOFT_H - FONT_HEIGHT) / 2;

    if (left != NULL) {
        ILI9341_DrawString(8, sy, left, ILI9341_WHITE, ILI9341_NAVY);
    }
    if (right != NULL) {
        uint16_t w = (uint16_t)strlen(right) * FONT_WIDTH;
        ILI9341_DrawString(ILI9341_WIDTH - 8 - w, sy, right,
                           ILI9341_WHITE, ILI9341_NAVY);
    }
}

bool UI_BackTouched(uint16_t x, uint16_t y)
{
    /* 軟鍵列的右半邊都算 Back（觸控目標放大，好按）*/
    return (y >= UI_SOFT_Y) && (x >= ILI9341_WIDTH / 2);
}
