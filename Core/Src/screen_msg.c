#include "screen.h"
#include "screen_msg.h"
#include "screen_kb.h"
#include "ili9341.h"
#include "ui.h"
#include "font.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Messages — Discord 訊息收發
 *
 * 收：ESP32 每 3 秒輪詢 Discord，把新訊息放進它的佇列；vNetTask 每秒用
 *     "MSG?" 取件，存進 g_msgs[] ring buffer。這裡只負責顯示。
 * 發：按預設按鈕 → Msg_Send() 排入待送 → vNetTask 送 "SEND <text>"。
 *     按 Type → 打開螢幕鍵盤（screen_kb），打完 Send 才回呼到這裡送出。
 *
 * 重畫策略跟時鐘/天氣一樣：只有「總則數」或「送出狀態」變了才重畫，
 * 否則每個 UI tick 都畫會閃。
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* 這幾個定義必須跟 main.c 一致 */
#define MSG_MAX 8
#define MSG_LEN 40
extern char              g_msgs[MSG_MAX][MSG_LEN];
extern volatile uint32_t g_msg_n;
extern volatile uint8_t  g_msg_unread;
extern volatile uint8_t  g_send_result;   /* 0=閒置 1=送出中 2=成功 3=失敗 */
extern char              g_weather[];
void Msg_Send(const char *text);

#define MSG_Y   36                        /* 訊息列表起始 y */
#define BTN_Y   170                       /* 預設按鈕列 y */
#define BTN_H   34
#define BTN_N   4

typedef struct {
    uint16_t    x, w;
    uint16_t    color;
    const char *label;
} msgbtn_t;

static const msgbtn_t BTNS[BTN_N] = {
    {   4, 72, ILI9341_BLUE,    "Hi"   },
    {  84, 72, ILI9341_GREEN,   "OK"   },
    { 164, 72, ILI9341_ORANGE,  "Info" },
    { 244, 72, ILI9341_MAGENTA, "Type" },
};

static uint32_t shown_n;        /* 上次畫出時的總則數 */
static uint8_t  shown_send;     /* 上次畫出的送出狀態 */

/* 畫一行訊息："user: text" —— 使用者名稱用青色，內容白色 */
static void draw_row(int row, const char *s)
{
    uint16_t y = MSG_Y + row * FONT_HEIGHT;
    ILI9341_FillRect(0, y, ILI9341_WIDTH, FONT_HEIGHT, ILI9341_BLACK);
    if (s == NULL || s[0] == '\0') return;

    const char *colon = strchr(s, ':');
    if (colon != NULL && (colon - s) < 20) {
        int nlen = (int)(colon - s) + 1;          /* 含冒號 */
        char name[24];
        if (nlen > (int)sizeof(name) - 1) nlen = (int)sizeof(name) - 1;
        memcpy(name, s, nlen);
        name[nlen] = '\0';
        ILI9341_DrawString(2, y, name, ILI9341_CYAN, ILI9341_BLACK);
        ILI9341_DrawString(2 + nlen * FONT_WIDTH, y, colon + 1,
                           ILI9341_WHITE, ILI9341_BLACK);
    } else {
        ILI9341_DrawString(2, y, s, ILI9341_WHITE, ILI9341_BLACK);
    }
}

/* 把最近 MSG_MAX 則畫出來，舊的在上、新的在下 */
static void draw_list(void)
{
    uint32_t n     = g_msg_n;
    uint32_t start = (n > MSG_MAX) ? (n - MSG_MAX) : 0;   /* 從第幾則開始顯示 */

    for (int row = 0; row < MSG_MAX; row++) {
        uint32_t k = start + row;
        draw_row(row, (k < n) ? g_msgs[k % MSG_MAX] : NULL);
    }
    if (n == 0)
        UI_DrawCentered(MSG_Y + 3 * FONT_HEIGHT, "(no messages)",
                        ILI9341_LIGHTGRAY, ILI9341_BLACK);
}

static void draw_buttons(void)
{
    for (int i = 0; i < BTN_N; i++) {
        ILI9341_FillRect(BTNS[i].x, BTN_Y, BTNS[i].w, BTN_H, BTNS[i].color);
        uint16_t w  = (uint16_t)strlen(BTNS[i].label) * FONT_WIDTH;
        uint16_t tx = BTNS[i].x + (BTNS[i].w - w) / 2;
        uint16_t ty = BTN_Y + (BTN_H - FONT_HEIGHT) / 2;
        ILI9341_DrawString(tx, ty, BTNS[i].label, ILI9341_WHITE, BTNS[i].color);
    }
}

/* 送出狀態顯示在軟鍵列左側 */
static void draw_status(void)
{
    const char *txt;
    uint16_t    col;
    switch (g_send_result) {
        case 1:  txt = "sending..."; col = ILI9341_YELLOW; break;
        case 2:  txt = "sent";       col = ILI9341_GREEN;  break;
        case 3:  txt = "send fail";  col = ILI9341_RED;    break;
        default: txt = "";           col = ILI9341_WHITE;  break;
    }
    uint16_t sy = UI_SOFT_Y + (UI_SOFT_H - FONT_HEIGHT) / 2;
    ILI9341_FillRect(0, UI_SOFT_Y, 110, UI_SOFT_H, ILI9341_NAVY);
    if (txt[0] != '\0')
        ILI9341_DrawString(8, sy, txt, col, ILI9341_NAVY);
}

static void msg_enter(void)
{
    UI_DrawFrame("Messages", NULL, "Back");
    draw_buttons();

    g_msg_unread = 0;              /* 進來就算已讀 */
    shown_n      = 0xFFFFFFFFu;    /* 強制重畫列表 */
    shown_send   = 0xFF;
}

/* 鍵盤按下 Send 後的回呼（此時鍵盤已經 pop 掉，我們又是最上層畫面）*/
static void kb_done(const char *text)
{
    if (text != NULL && text[0] != '\0')
        Msg_Send(text);
}

static void msg_touch(uint16_t x, uint16_t y)
{
    if (UI_BackTouched(x, y)) { Screen_Pop(); return; }

    if (y < BTN_Y || y >= BTN_Y + BTN_H) return;

    for (int i = 0; i < BTN_N; i++) {
        if (x < BTNS[i].x || x >= BTNS[i].x + BTNS[i].w) continue;

        if (i == 0) {
            Msg_Send("Hi from the STM32 handheld!");
        } else if (i == 1) {
            Msg_Send("OK");
        } else if (i == 2) {
            /* 帶上裝置實際狀態，證明訊息真的來自這台機器 */
            uint32_t up = xTaskGetTickCount() / configTICK_RATE_HZ;
            char info[64];
            snprintf(info, sizeof(info), "up %lus | %s",
                     (unsigned long)up, g_weather[0] ? g_weather : "no weather");
            Msg_Send(info);
        } else {
            /* 鍵盤按 Send 才會回呼 kb_done；按 Back 就什麼都不做 */
            Keyboard_Open("Message", "", kb_done);
        }
        return;
    }
}

static void msg_render(void)
{
    if (g_msg_n != shown_n) {
        shown_n      = g_msg_n;
        g_msg_unread = 0;          /* 停留在這個畫面時，收到即已讀 */
        draw_list();
    }
    if (g_send_result != shown_send) {
        shown_send = g_send_result;
        draw_status();
    }
}

void ScreenMsg_Register(void)
{
    Screen_Register(SCREEN_MSG, (screen_t){
        .on_enter  = msg_enter,
        .on_touch  = msg_touch,
        .on_render = msg_render,
    });
}
