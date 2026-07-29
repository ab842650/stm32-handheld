/* 可載入 module 的連結腳本（M2a：位置無關 ROPI）
 *
 * 連結位址改成 0 —— 象徵「這份碼位置無關，載到哪都能跑」。
 * 配 -mropi：碼 + rodata 全改成 PC 相對引用，不再有指向 0x2001Cxxx 的
 * 絕對位址。loader 仍把 .bin 讀進 0x2001C000 執行，但因為位置無關，
 * 就算連結在 0、跑在 0x2001C000 也正確。
 *
 * .entry 排最前面 → module_main 落在 .bin 的 offset 0。
 */
MEMORY
{
  MOD (rwx) : ORIGIN = 0x00000000, LENGTH = 16K
}

ENTRY(module_main)

SECTIONS
{
  .text 0x00000000 :
  {
    KEEP(*(.entry))     /* module_main 保證在最前面 */
    *(.text*)
    *(.rodata*)
  } > MOD

  /* M2a 前提：module 不能有可寫全域變數（.data/.bss）。
   * 若之後遊戲需要狀態，要進 M2b：-mrwpi + loader 設 r9。
   * 這裡故意不放 .data/.bss，讓「不小心加了全域變數」會連結報錯提醒你。 */
  .data : { KEEP(*(.data*)) *(.got*) } > MOD
  .bss  : { *(.bss*) *(COMMON) } > MOD
}
