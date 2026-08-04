#ifndef SCREEN_KB_H
#define SCREEN_KB_H

#include <stdint.h>

/* On-screen keyboard, shared by any screen that needs text input:
 *
 *     static void got_text(const char *s) { Msg_Send(s); }
 *     Keyboard_Open("Message", "", got_text);
 *
 * Opening pushes the keyboard over the caller. Send pops it and then invokes
 * on_done, in that order, so the callback may push a further screen. Back pops
 * without calling on_done. */

#define KB_MAX  60      /* max characters, excluding the terminator */

typedef void (*kb_done_fn)(const char *text);

/**
 * @param initial  pre-filled text, NULL or "" for empty
 * @param on_done  called on Send; NULL to discard the result
 */
void Keyboard_Open(const char *title, const char *initial, kb_done_fn on_done);

void ScreenKb_Register(void);

#endif /* SCREEN_KB_H */
