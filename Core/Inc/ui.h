#ifndef UI_H
#define UI_H

#include "ili9341.h"
#include <stdint.h>
#include <stdbool.h>

/* Shared layout, feature-phone style:
 *
 *   y=0            +--------------+
 *                  |    title     |  navy
 *   UI_CONTENT_Y   +--------------+
 *                  |              |  each screen draws its own content
 *   UI_SOFT_Y      +--------------+
 *                  | left   right |  softkeys, navy
 *   HEIGHT         +--------------+
 */
#define UI_TITLE_H    32
#define UI_SOFT_H     30
#define UI_SOFT_Y     (ILI9341_HEIGHT - UI_SOFT_H)   /* 210 */
#define UI_CONTENT_Y  UI_TITLE_H                     /* 32 */
#define UI_CONTENT_H  (UI_SOFT_Y - UI_TITLE_H)       /* 178 */

/** One line of text, horizontally centred on the screen. */
void UI_DrawCentered(uint16_t y, const char *s, uint16_t fg, uint16_t bg);

/** Clear the screen and draw the title + softkey bars. NULL softkey = omit. */
void UI_DrawFrame(const char *title, const char *left, const char *right);

/** True if the touch landed in the Back softkey area. */
bool UI_BackTouched(uint16_t x, uint16_t y);

#endif /* UI_H */
