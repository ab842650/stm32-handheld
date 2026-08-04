#ifndef UI_EVENT_H
#define UI_EVENT_H

#include <stdint.h>

typedef enum {
    UI_EVT_TOUCH_DOWN,   /* touch began, carries coordinates */
    UI_EVT_TOUCH_UP,     /* finger lifted */
    UI_EVT_BUTTON,       /* physical button */
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    uint16_t        x;   /* pixels, 0..319 */
    uint16_t        y;   /* pixels, 0..239 */
} ui_event_t;

#endif /* UI_EVENT_H */
