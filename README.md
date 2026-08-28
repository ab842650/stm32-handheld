# STM32 Bare-Metal Handheld

A from-scratch "phone-like" touch handheld built **bare-metal** on an STM32F407
(Cortex-M4 @ 168 MHz, 1 MB Flash, 192 KB RAM) with FreeRTOS.

It started as a touch UI and grew three layers that would normally be somebody
else's job: an **OS layer** (a loader that runs native code from the SD card, a
syscall interface, position-independent modules), **emulators** (CHIP-8, and a
Game Boy running Pokémon Red at ~60 fps through a hand-written ROM cache), and a
**network stack** (an ESP32-S3 WiFi co-processor speaking a custom UART
protocol, used to send and receive Discord messages). Every driver and every
peripheral init is hand-written.

> Built to understand — by rebuilding them — how program loading, syscalls,
> position-independent code, virtual machines, CPU caches, and filesystem
> concurrency actually work, on hardware with **no MMU and no CPU cache**.

**▶ [Watch the demo video](https://drive.google.com/file/d/1G74l75u3DNy1aKpoiGos22fDyOqNpNuh/view?usp=drive_link)**
— the home menu and apps, a typed message going out to Discord over WiFi,
Pokémon Red, and a native game module loading off the SD card.

## Hardware

| | |
|---|---|
| MCU | STM32F407VGT6 — Cortex-M4, 168 MHz, FPU, 1 MB Flash, 128 KB SRAM + 64 KB CCM |
| Display | ILI9341 320×240 RGB565 over SPI1, DMA-driven |
| Touch | XPT2046 resistive, shares SPI1, PENIRQ via EXTI |
| Storage | microSD over SPI2 + FatFs |
| WiFi | ESP32-S3 co-processor over USART3 (PB10/PB11), 115200 8N1 |

Flash use is ~136 KB of 1 MB. RAM is the constrained resource, which is why the
memory map is used deliberately (below).

## Apps

| App | What it does |
|---|---|
| **Messages** | Two-way Discord chat — 8-slot receive ring, presets or keyboard to send, unread badge on the home screen |
| **Game Boy** | Full emulator: ROM browser, on-screen D-pad, `.sav` battery saves with debounced autosave |
| **Game** | Launcher for loadable native modules — Snake, Tetris, CHIP-8 |
| **Photo** | Full-screen viewer for 24-bit BMP and baseline JPEG, with album paging |
| **Notes** | Browse `.txt` files on the card and append to them |
| **Calculator** | Touch keypad, running expression display |

Shared across screens: an on-screen keyboard as a reusable component — three
layers, one-shot shift, blinking cursor, opened by any screen with
`Keyboard_Open(title, initial, callback)` — an NTP-synced clock and live weather
in the home title bar, and loadable modules, where dropping a `.BIN` on the card
makes it appear in the launcher with no firmware rebuild.

## Architecture

```
XPT2046 PENIRQ ──EXTI──> InputTask (prio 4) ──queue──> UITask (prio 3)
                                                          │
USART3 RX ──ISR──> ring buffer ──> NetTask (prio 2) ──────┘  (shared globals)
```

| Task | Priority | Stack | Role |
|---|---|---|---|
| `InputTask` | 4 | 256 w | Debounce, one event per press |
| `UITask` | 3 | 2560 w | All rendering, the screen stack, emulators |
| `NetTask` | 2 | 1024 w | Owns the ESP32 link; never blocks the UI |

Every wait blocks rather than spins — queue, semaphore, mutex, and a DMA
completion semaphore inside the display driver — so the CPU idles during
transfers instead of burning cycles. Screens register `on_enter / on_touch /
on_render` and are pushed and popped like a navigation stack, with change-driven
redraws: the clock repaints only when the second changes.

**Memory map, used deliberately**

| Region | Holds | Why |
|---|---|---|
| Flash (1 MB) | Code, fonts | Emulator cores are far too big for the module region |
| SRAM (128 KB) | RTOS heap, DMA buffers, cart RAM | **DMA cannot reach CCM** |
| CCM (64 KB) | Emulator working RAM, ROM cache | Fast, and otherwise unused |

CCM being neither DMA-capable nor a good fit for everything is exactly why the
loader and the ROM cache are shaped the way they are.

The project also deliberately **does not regenerate with CubeMX**: peripherals
added by hand (SPI2 for the card, USART3 for WiFi) live outside the `USER CODE`
markers, and a regen would wipe the display and touch pin setup.

## Engineering highlights

### A software CPU cache made Pokémon playable

Pokémon Red is 1 MB — it cannot live in RAM, so ROM reads are served from the SD
card. The obvious design, one 16 KB bank slot, thrashed: the game's working set
is scattered, so nearly every access missed and cost a ~12 ms bank load.

Replacing it with a **32-line × 512 B direct-mapped cache** in CCM changed the
granularity — a miss now costs one ~1 ms read, and 32 lines hold the scattered
hot spots simultaneously. **Single-digit fps → ~60 fps.** A hands-on lesson in
line size, working set and locality, on a chip with no cache of its own.

### Diagnosing real filesystem corruption

Symptom: Pokémon froze at the intro and the FPS counter read in the tens of
thousands. The chain: instrument the ROM reader → SD reads were returning
`FR_INT_ERR` → the cache was tagging lines valid even when the read failed, so
it served garbage forever → `chkdsk` on a PC named the exact files as corrupt,
confirming the damage was on the card, not in RAM. Root cause: `_FS_REENTRANT`
was `0`, and a download in `NetTask` had been racing `UITask`'s reads, clobbering
the shared FatFs window buffer and writing garbage into the on-disk FAT.

The fix enables FatFs's own volume lock rather than hand-wrapping eleven call
sites, because a hand-rolled mutex can be forgotten at one site — and this one
had already proven it would corrupt the card silently.

**Then it was proven.** A stress test runs two equal-priority tasks that write a
pattern, read it back and byte-compare; equal priority plus time slicing gets
them preempted *inside* `f_*` calls. The key detail is the instrumentation: the
lock's acquire path first tries a zero-timeout take, so a failure counts as
*genuine contention* — distinguishing "the lock works" from "nothing ever
actually raced". Result over ~10,100 iterations: **10,206 real contentions, 0
mismatches, 0 failures**, and a clean `chkdsk` afterwards. Still in the tree
behind `#define SD_STRESS_TEST`.

### Runtime memory instrumentation

Repeated stack overflows — one of which killed touch permanently after every
download — prompted proper tooling rather than guesswork:
`configCHECK_FOR_STACK_OVERFLOW=2` with a hook that names the offending task and
halts, periodic per-task high-water marks, and `-fstack-usage` at build time.
That case was a `FIL` (~560 B) plus a 512 B buffer on a 2 KB task stack.

## Repository layout

```
Core/            main.c, screens (home/calc/notes/photo/game/gb/msg/keyboard)
Drivers/BSP/     ili9341.c/.h, xpt2046.c/.h   (hand-written)
Middlewares/     FatFs, TJpgDec (JPEG), Peanut-GB (Game Boy)
FATFS/           SD-over-SPI glue (diskio)
esp32/           ESP32-S3 co-processor firmware — see esp32/README.md
tools/           Python asset generators; module/ = position-independent .bin build
STM32F407VGTX_FLASH.ld    linker script (reserved regions, CCM sections)
```

## Building

**Firmware** — open in STM32CubeIDE, build and flash.

**Co-processor** — open `esp32/wifi_bridge/` in the Arduino IDE. It needs the
esp32 board package and ArduinoJson 7.x, and `USB CDC On Boot` must be
*Disabled*. Copy `secrets.h.example` to `secrets.h` and fill in your WiFi and
Discord credentials; `secrets.h` is gitignored. Full setup notes and the
protocol table are in [esp32/README.md](esp32/README.md).

**Modules** — `tools/module/build.sh <src.c>` compiles a position-independent
module to `<NAME>.BIN`; copy it to `/GAMES` on the card.

**Assets** — `tools/*.py` generate fonts, icons and test images.

> **ROMs are not included.** Commercial Game Boy ROMs are copyrighted. Use your
> own homebrew or public-domain `.gb` files. CHIP-8 public-domain ROMs go in
> `/ROMS`.

## SD card layout

```
/GAMES/*.BIN     native game modules (SNAKE.BIN, TETRIS.BIN, CHIP8.BIN, ...)
/ROMS/*.ch8      CHIP-8 ROMs
/GB/*.gb         Game Boy ROMs (+ .sav save files, written by the emulator)
/PHOTOS/*.jpg    images for the photo viewer
/NOTES/*.txt     text files for Notes
```
