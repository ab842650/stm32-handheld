#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

/* 16x16 1-bpp icons: 2 bytes per row (MSB leftmost), 16 rows = 32 bytes. */
#define ICON_SIZE  16

extern const uint8_t icon_calc[32];
extern const uint8_t icon_game[32];
extern const uint8_t icon_notes[32];
extern const uint8_t icon_clock[32];
extern const uint8_t icon_photo[32];
extern const uint8_t icon_msg[32];

#endif /* ICONS_H */
