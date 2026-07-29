#!/usr/bin/env bash
# 把一個 module 的 .c 編成板子可載入的 HELLO.BIN
#   - 連結到固定位址 0x2001C000（見 module.ld）
#   - float ABI / cpu 跟主韌體完全一致（呼叫慣例才對得上）
#   - objcopy 成 raw binary（.bin 裡就是純機器碼，offset 0 = module_main）
#
# 用法：  ./build.sh [module_hello.c]
set -e

# STM32CubeIDE 12.3 內建的 ARM 工具鏈
TOOLS="/c/ST/STM32CubeIDE_1.17.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.1.0.202410251130/tools/bin"
GCC="$TOOLS/arm-none-eabi-gcc"
OBJCOPY="$TOOLS/arm-none-eabi-objcopy"
OBJDUMP="$TOOLS/arm-none-eabi-objdump"

SRC="${1:-module_hello.c}"
NAME="$(basename "$SRC" .c)"

CFLAGS="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
        -Os -ffreestanding -nostdlib -fno-exceptions \
        -ffunction-sections -fdata-sections \
        -fpic -fno-jump-tables \
        -fno-tree-loop-distribute-patterns \
        -I ../../Core/Inc"     # 找得到 module_api.h（跟主韌體共用契約）
# -fno-tree-loop-distribute-patterns : 別把清零/複製迴圈轉成 memset/memcpy
#   （module 是 -nostdlib，沒有 libc，那些呼叫會連結失敗）
# -fpic         : GCC 的位置無關碼 —— 引用自己的 rodata 改成「載入偏移 + 加 PC」
#                 （本模組無全域變數/外部符號 → 不需 GOT、不需設 r9）
# -fno-jump-tables : 避免 switch 產生絕對位址跳表（會破壞位置無關）

cd "$(dirname "$0")"

OUT="$(echo "$NAME" | tr 'a-z' 'A-Z').BIN"    # snake.c → SNAKE.BIN

"$GCC" $CFLAGS -T module.ld -Wl,--gc-sections -Wl,-e,module_main \
       -o "$NAME.elf" "$SRC"
"$OBJCOPY" -O binary "$NAME.elf" "$OUT"

echo "== $OUT 產出 =="
ls -l "$OUT"
echo "== 確認 module_main 在 offset 0（.entry）、無殘留重定位 =="
"$OBJDUMP" -d "$NAME.elf" | sed -n '/<module_main>:/,/bx\|pop/p' | head -6
"$TOOLS/arm-none-eabi-readelf" -r "$NAME.elf" | grep -iE 'reloc|no relocations' | head -2
