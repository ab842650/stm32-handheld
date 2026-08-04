#ifndef SCREEN_H
#define SCREEN_H
#include <stdint.h>

typedef enum {
      SCREEN_HOME,
      SCREEN_CALC,
      SCREEN_PHOTO,
      SCREEN_GAME,
      SCREEN_CLOCK,
      SCREEN_NOTES,
      SCREEN_GB,
      SCREEN_MSG,
      SCREEN_KB,
      SCREEN_COUNT,
 } screen_id_t;

typedef struct{
	void (*on_enter)(void);
	void (*on_exit)(void);
	void (*on_touch)(uint16_t x, uint16_t y);
	void (*on_render)(void);
}screen_t;

screen_id_t Screen_Current(void);
void Screen_Register(screen_id_t id, screen_t screen);
void Screen_Push(screen_id_t id);
void Screen_Pop(void);
void Screen_Replace(screen_id_t id);
void Screen_OnTouch(uint16_t x, uint16_t y);
void Screen_OnRender(void);

#endif /* SCREEN_H */
