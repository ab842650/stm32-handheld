#ifndef LOADER_H
#define LOADER_H

/* 從 SD 卡載入一個 module（raw .bin）進預留的 RAM 區並執行。
 * 回傳 module 的回傳值；載入失敗回 -1。 */
int Loader_RunModule(const char *path);

#endif /* LOADER_H */
