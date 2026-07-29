#include "screen.h"
#include <stddef.h>

#define SCREEN_STACK_MAX 8

static screen_t screens[SCREEN_COUNT];

static screen_id_t stack[SCREEN_STACK_MAX];
static int stack_top = -1;   /* -1 = 空 */

screen_id_t Screen_Current(void){
	return stack[stack_top];
}

void Screen_Register(screen_id_t id, screen_t screen){
	if (id >= SCREEN_COUNT) return;
	screens[id] = screen;
}

void Screen_Push(screen_id_t id){
	if (id >= SCREEN_COUNT) return;
	if (stack_top >= SCREEN_STACK_MAX - 1) return;

	if (stack_top >= 0 && screens[stack[stack_top]].on_exit != NULL)
		screens[stack[stack_top]].on_exit();

	stack[++stack_top] = id;

	if (screens[id].on_enter != NULL)
		screens[id].on_enter();
}

void Screen_Pop(void){
	if (stack_top <= 0) return;   /* 最底層不能再彈出 */

	if (screens[stack[stack_top]].on_exit != NULL)
		screens[stack[stack_top]].on_exit();

	stack_top--;

	if (screens[stack[stack_top]].on_enter != NULL)
		screens[stack[stack_top]].on_enter();
}

void Screen_Replace(screen_id_t id){
	if (id >= SCREEN_COUNT) return;

	if (stack_top >= 0 && screens[stack[stack_top]].on_exit != NULL)
		screens[stack[stack_top]].on_exit();

	if (stack_top < 0)
		stack_top = 0;

	stack[stack_top] = id;

	if (screens[id].on_enter != NULL)
		screens[id].on_enter();
}

void Screen_OnTouch(uint16_t x, uint16_t y){
	if (stack_top < 0) return;
	screen_id_t id = stack[stack_top];
	if (screens[id].on_touch != NULL)
		screens[id].on_touch(x, y);
}

void Screen_OnRender(void){
	if (stack_top < 0) return;
	screen_id_t id = stack[stack_top];
	if (screens[id].on_render != NULL)
		screens[id].on_render();
}
