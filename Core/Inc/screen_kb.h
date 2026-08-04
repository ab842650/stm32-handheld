#ifndef SCREEN_KB_H
#define SCREEN_KB_H

#include <stdint.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 螢幕鍵盤 —— 共用元件
 *
 * 用法（呼叫者不需要知道鍵盤畫面長怎樣）：
 *
 *     static void got_text(const char *s) { Msg_Send(s); }
 *     ...
 *     Keyboard_Open("Message", "", got_text);
 *
 * 鍵盤是一個獨立畫面，Keyboard_Open 會把它 push 到畫面堆疊上蓋住呼叫者。
 * 按 Send  → 先 Screen_Pop()（呼叫者的 on_enter 會重畫）再呼叫 on_done。
 *            先 pop 再回呼，代表 on_done 裡面還可以安全地再 Screen_Push。
 * 按 Back  → 只 Screen_Pop()，不呼叫 on_done（＝取消）。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define KB_MAX  60      /* 可輸入的最大字元數（不含結尾 '\0'）*/

typedef void (*kb_done_fn)(const char *text);

/**
 * @brief 打開鍵盤
 * @param title    標題列文字
 * @param initial  預填內容，NULL 或 "" = 空白
 * @param on_done  按 Send 時的回呼；NULL = 只是給使用者打好玩的
 */
void Keyboard_Open(const char *title, const char *initial, kb_done_fn on_done);

void ScreenKb_Register(void);

#endif /* SCREEN_KB_H */
