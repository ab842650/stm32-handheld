#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

/* 16x16 單色圖示，每列 2 bytes（MSB 在左），共 16 列 = 32 bytes */
#define ICON_SIZE  16

extern const uint8_t icon_calc[32];
extern const uint8_t icon_game[32];
extern const uint8_t icon_notes[32];
extern const uint8_t icon_clock[32];
extern const uint8_t icon_photo[32];
extern const uint8_t icon_msg[32];

#endif /* ICONS_H */
