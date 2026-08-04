# STM32 Bare-Metal Handheld — Touch UI, Loadable Apps & a Game Boy Emulator

A from-scratch "phone-like" handheld built **bare-metal** on an STM32F407
(Cortex-M4 @ 168 MHz, 1 MB Flash, 192 KB RAM) with FreeRTOS. It started as a
touch UI and grew into a small **operating-system layer**: a loader that runs
native code from the SD card, a system-call interface, position-independent
modules, and emulators — including a **Game Boy emulator running Pokémon Red at
~60 fps** through a hand-written ROM cache.

> Built as a self-driven learning project to understand — by rebuilding them —
> how program loading, system calls, position-independent code, virtual
> machines, and CPU caches actually work, on hardware with **no MMU and no CPU
> cache**.

## Highlights

- **Game Boy emulator** (Peanut-GB) as a built-in app — 160×144 display,
  on-screen touch D-pad + buttons, ROMs streamed from SD. Profiled and optimized
  **13 → 60 fps** (compiler flags, disabling unused PPU accuracy, resolution
  trade-offs).
- **Software cache for large (MBC) ROM streaming** — a 32-line × 512 B
  direct-mapped cache in CCM RAM took **Pokémon Red (1 MB) from single-digit fps
  to ~60 fps** by fitting the game's scattered working set instead of thrashing
  a single 16 KB bank slot. A hands-on lesson in cache granularity, working set,
  and locality.
- **Loadable-module system (a mini-OS)** — load native `.bin` code from SD into
  RAM and execute it: a hand-written **program loader**, a **syscall table**, and
  **position-independent code** (`-fpic`) so modules run at any address *without
  an MMU*.
- **CHIP-8 emulator** module — one module runs many ROMs from `/ROMS`.
- **Native game "cartridges"** — Snake and Tetris compiled as position-independent
  `.bin` files on the SD card; add a game by dropping in a new `.bin`.
- **Apps** — calculator, clock, notes viewer, photo viewer (24-bit BMP + baseline
  JPEG via TJpgDec).
- **Drivers written from the registers up** — ILI9341 (SPI + DMA), XPT2046
  resistive touch (SPI + EXTI), SD-over-SPI + FatFs.

## Hardware

| | |
|---|---|
| MCU | STM32F407VGT6 (Cortex-M4, 168 MHz, FPU, 1 MB Flash, 128 KB SRAM + 64 KB CCM) |
| Display | ILI9341 320×240 RGB565 over SPI1 (+ DMA) |
| Touch | XPT2046 resistive, SPI1 shared bus, IRQ via EXTI |
| Storage | microSD over SPI2 + FatFs |

## Architecture

- **FreeRTOS**: touch ISR → task notification → input task (debounce) → queue →
  UI task (screen stack). Blocking primitives (queue / semaphore / mutex) let the
  CPU sleep instead of busy-wait; DMA completion wakes the drawing task.
- **Screen stack** with `on_enter / on_touch / on_render` callbacks per screen.
- **Hand-written peripheral init** (the project intentionally does *not* use CubeMX
  regeneration; peripherals like SPI2 for the SD card are added by hand).
- **Memory map** put to work: code executes from Flash; RTOS heap and DMA buffers
  live in SRAM; the emulator's working RAM and the ROM cache live in the 64 KB CCM
  (fast, but not DMA- or execute-capable — which is exactly why the loader and the
  cache are designed the way they are).

## The "OS mechanisms from scratch" thread

The project's spine is a single question — *why can't an MCU keep programs on
storage and load them like a PC?* — answered by building the pieces by hand:

```
program loader  →  syscall table  →  position-independent code (no MMU)
   →  bytecode VM / emulators (CHIP-8, Game Boy)  →  a software CPU cache
```

The full engineering log (decisions, bugs, and the reasoning behind each step) is
in **[DEV_LOG.md](DEV_LOG.md)**.

## Repository layout

```
Core/            main.c, screens (home/calc/notes/photo/game/gb/msg/keyboard), drivers glue
Drivers/BSP/     ili9341.c/.h, xpt2046.c/.h  (hand-written)
Middlewares/     FatFs, TJpgDec (JPEG), Peanut-GB (Game Boy)
FATFS/           SD-over-SPI glue (diskio)
esp32/           ESP32-S3 WiFi co-processor firmware (Arduino) — see esp32/README.md
tools/           Python asset generators; module/ = position-independent .bin build
STM32F407VGTX_FLASH.ld   linker script (reserved regions, CCM sections)
```

## Building

Open in **STM32CubeIDE**, build and flash. Loadable modules and emulator ROMs are
separate from the firmware:

- `tools/module/build.sh <src.c>` — compiles a position-independent module to
  `<NAME>.BIN` (put it in `/GAMES` on the SD card).
- `tools/*.py` — generate fonts, icons, and test images.

**ROMs are not included.** Commercial Game Boy ROMs are copyrighted — use your own
homebrew / public-domain `.gb` files (place in `/GB` on the SD card). CHIP-8 public-
domain ROMs go in `/ROMS`.

## SD card layout

```
/GAMES/*.BIN     native game modules (SNAKE.BIN, TETRIS.BIN, CHIP8.BIN, ...)
/ROMS/*.ch8      CHIP-8 ROMs
/GB/*.gb         Game Boy ROMs
/PHOTOS/*.jpg    images for the photo viewer
/NOTES/*.txt     text files for the notes viewer
```
