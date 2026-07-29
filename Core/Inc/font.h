#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* 8x16 ASCII bitmap font, chars 0x20..0x7E */
#define FONT_WIDTH   8
#define FONT_HEIGHT  16
#define FONT_FIRST   0x20
#define FONT_LAST    0x7E

extern const uint8_t font8x16[95][16];

#endif /* FONT_H */
