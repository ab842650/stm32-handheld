#ifndef UI_EVENT_H
#define UI_EVENT_H

#include <stdint.h>

typedef enum {
    UI_EVT_TOUCH_DOWN,   /* 觸碰開始，帶座標 */
    UI_EVT_TOUCH_UP,     /* 離開螢幕 */
    UI_EVT_BUTTON,       /* 實體按鈕 */
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    uint16_t        x;   /* 像素座標 (0~319) */
    uint16_t        y;   /* 像素座標 (0~239) */
} ui_event_t;

#endif /* UI_EVENT_H */
