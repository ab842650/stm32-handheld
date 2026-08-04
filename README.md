# STM32 Bare-Metal Handheld

A from-scratch "phone-like" touch handheld built **bare-metal** on an STM32F407
(Cortex-M4 @ 168 MHz, 1 MB Flash, 192 KB RAM) with FreeRTOS.

It started as a touch UI and grew into three layers that would normally be
somebody else's job: a small **operating-system layer** (a loader that runs
native code from the SD card, a syscall interface, position-independent
modules), **emulators** (CHIP-8 and a Game Boy running Pokémon Red at ~60 fps
through a hand-written ROM cache), and a **network stack** (an ESP32-S3 WiFi
co-processor speaking a custom UART protocol, used to send and receive Discord
messages).

> Built as a self-driven learning project to understand — by rebuilding them —
> how program loading, system calls, position-independent code, virtual
> machines, CPU caches, and filesystem concurrency actually work, on hardware
> with **no MMU and no CPU cache**.

Every driver and every peripheral init is hand-written. The engineering log,
including the bugs and the reasoning behind each decision, is in
**[DEV_LOG.md](DEV_LOG.md)**.

---

## Contents

- [Features](#features) · [Hardware](#hardware) · [Architecture](#architecture)
- [Technologies and topics](#technologies-and-topics)
- [Engineering highlights](#engineering-highlights)
- [Building](#building) · [SD card layout](#sd-card-layout)

---

## Features

### Apps

| App | What it does |
|---|---|
| **Messages** | Two-way Discord chat. Receives messages into an 8-slot ring, shows sender and text, sends via presets or the keyboard. Unread badge on the home screen. |
| **Game Boy** | Full emulator. ROM browser, on-screen D-pad and buttons, battery-save `.sav` files with debounced autosave. |
| **Game** | Launcher for loadable native modules — Snake, Tetris, CHIP-8. |
| **Photo** | Full-screen viewer for 24-bit BMP and baseline JPEG, with album paging. |
| **Notes** | Browse `.txt` files on the card, and append to them with the on-screen keyboard. |
| **Calculator** | Touch keypad, running expression display, left-to-right evaluation. |

### System

- **On-screen keyboard** — a reusable component, not tied to one screen.
  Three layers (lower / upper / symbols), one-shot shift, blinking cursor.
  Any screen calls `Keyboard_Open(title, initial, callback)`.
- **Home screen title bar** — live NTP-synced clock and current weather.
- **Loadable modules** — drop a `.BIN` on the SD card and it appears in the
  launcher. The firmware does not change.
- **Networking** — NTP time sync, HTTP GET, and HTTPS via the co-processor.

---

## Hardware

| | |
|---|---|
| MCU | STM32F407VGT6 — Cortex-M4, 168 MHz, FPU, 1 MB Flash, 128 KB SRAM + 64 KB CCM |
| Display | ILI9341 320×240 RGB565 over SPI1, DMA-driven |
| Touch | XPT2046 resistive, shares SPI1, PENIRQ via EXTI |
| Storage | microSD over SPI2 + FatFs |
| WiFi | ESP32-S3 co-processor over USART3 (PB10/PB11), 115200 8N1 |

Flash use is ~136 KB of 1 MB; RAM is the constrained resource, which is why the
memory map is used deliberately (see below).

---

## Architecture

### Task structure

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
transfers instead of burning cycles.

### Screen stack

Each screen registers `on_enter / on_touch / on_render` callbacks and is pushed
and popped like a navigation stack. Redraws are change-driven: the clock only
repaints when the second changes, Messages only when the message count does.

### Memory map, used deliberately

| Region | Holds | Why |
|---|---|---|
| Flash (1 MB) | Code, fonts | Emulator cores are far too big for the module region |
| SRAM (128 KB) | RTOS heap, DMA buffers, cart RAM | **DMA cannot reach CCM** |
| CCM (64 KB) | Emulator working RAM, ROM cache | Fast, and otherwise unused |

CCM being neither DMA-capable nor a good fit for everything is exactly why the
loader and the ROM cache are shaped the way they are.

### Peripheral init is hand-written

The project deliberately **does not regenerate with CubeMX**. Peripherals added
by hand (SPI2 for the card, USART3 for WiFi) live outside the `USER CODE`
markers, and a regen would wipe the display and touch pin setup.

---

## Technologies and topics

**Bare-metal / drivers**

- ILI9341 command set, MADCTL orientation bits, gamma and power registers
- Windowed pixel writes (CASET/PASET/RAMWR) instead of per-pixel addressing
- SPI with software NSS; **runtime baud-rate switching** on a shared bus
  (42 MHz for the display, 1.3 MHz for the touch controller)
- DMA (memory-to-peripheral) with completion signalled by a semaphore from ISR
- EXTI, NVIC priority grouping, and the FreeRTOS syscall-priority threshold
- 1-bpp bitmap fonts and icons, integer-scaled glyph rendering

**RTOS**

- Task design, priorities, blocking primitives, time slicing
- ISR-to-task handoff: task notifications and `...FromISR` APIs
- Stack sizing, `-fstack-usage`, `uxTaskGetStackHighWaterMark`,
  `configCHECK_FOR_STACK_OVERFLOW` and an overflow hook

**Storage**

- SD over SPI, FatFs port, `diskio` glue
- **FatFs re-entrancy**: volume locking via `_FS_REENTRANT` with a FreeRTOS
  mutex, and `_FS_LOCK` for simultaneously open files
- Streaming large files from the card under a cache

**OS mechanisms**

- Program loader, ABI design, syscall table
- Position-independent code (`-fpic`), Thumb bit, `__DSB()`/`__ISB()` before
  executing freshly written memory
- Executing from SRAM without an MMU

**Emulation**

- CHIP-8 interpreter (opcode decode, 60 Hz timers, keypad)
- Game Boy via Peanut-GB; MBC bank switching served from a software cache
- Profiling and optimisation: 13 → 60 fps

**Networking**

- Co-processor architecture over UART; line-oriented text protocol
- Interrupt-driven RX with a **lock-free SPSC ring buffer**
- NTP, HTTP/1.0 (including close-delimited responses with no `Content-Length`),
  HTTPS via the ESP32, JSON, Discord REST API v10
- Flow control: a chunk-and-ack protocol so a fast sender cannot overrun a slow
  receiver

**Image decoding**

- BMP by hand (BGR order, bottom-up rows, 4-byte row padding)
- Baseline JPEG via TJpgDec with input/output callbacks, plus endianness
  conversion for the display

---

## Engineering highlights

### A software CPU cache made Pokémon playable

Pokémon Red is 1 MB — it cannot live in RAM, so ROM reads are served from the
SD card. The obvious design, one 16 KB bank slot, thrashed: the game's working
set is scattered, so nearly every access missed and cost a ~12 ms bank load.
Single-digit fps.

Replacing it with a **32-line × 512 B direct-mapped cache** in CCM changed the
granularity: a miss now costs one ~1 ms read, and 32 lines hold the scattered
hot spots simultaneously. **Single-digit fps → ~60 fps.**

A hands-on lesson in cache line size, working set, and locality — built by hand
on a chip that has no cache of its own.

### Diagnosing real filesystem corruption

Symptom: Pokémon froze at the intro and the FPS counter read in the tens of
thousands.

The chain: instrument the ROM reader → the SD reads were returning
`FR_INT_ERR` → the cache was tagging lines as valid even when the read failed,
so it served garbage forever → but *why* were reads failing? `chkdsk` on a PC
named the exact files as corrupt, confirming the damage was on the card, not in
RAM. Root cause: `_FS_REENTRANT` was `0`, and a download running in `NetTask`
had been racing `UITask`'s reads, clobbering the shared FatFs window buffer and
writing garbage into the on-disk FAT.

The fix was to enable FatFs's own volume lock rather than hand-wrap eleven call
sites, because a hand-rolled mutex can be forgotten at one site and this one
had already proven it would corrupt the card silently.

**Then it was proven.** A stress test runs two equal-priority tasks that write a
pattern, read it back and byte-compare; equal priority plus time slicing gets
them preempted *inside* `f_*` calls. The key detail is the instrumentation: the
lock's acquire path first tries a zero-timeout take, so a failure counts as
*genuine contention*. That distinguishes "the lock works" from "nothing ever
actually raced".

> Result: ~10,100 iterations, **10,206 real contentions, 0 mismatches, 0
> failures** — and a clean `chkdsk` afterwards.

The test is still in the tree behind `#define SD_STRESS_TEST`.

### Runtime memory instrumentation

Repeated stack overflows — one of which killed touch permanently after every
download — prompted proper tooling rather than guesswork:
`configCHECK_FOR_STACK_OVERFLOW=2` with a hook that names the offending task and
halts, per-task high-water marks printed periodically, and `-fstack-usage` at
build time. The root cause in that case was a `FIL` (~560 B) plus a 512 B buffer
placed on a 2 KB task stack.

### The "OS mechanisms from scratch" thread

The project's spine is one question — *why can't an MCU keep programs on
storage and load them like a PC?* — answered by building the pieces:

```
program loader → syscall table → position-independent code (no MMU)
   → bytecode VM / emulators (CHIP-8, Game Boy) → a software CPU cache
```

---

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
> own homebrew or public-domain `.gb` files. CHIP-8 public-domain ROMs go
> in `/ROMS`.

## SD card layout

```
/GAMES/*.BIN     native game modules (SNAKE.BIN, TETRIS.BIN, CHIP8.BIN, ...)
/ROMS/*.ch8      CHIP-8 ROMs
/GB/*.gb         Game Boy ROMs (+ .sav save files, written by the emulator)
/PHOTOS/*.jpg    images for the photo viewer
/NOTES/*.txt     text files for Notes
```

## Known limitations

- **ASCII only.** The bitmap font covers `0x20`–`0x7E`, so non-ASCII in Discord
  messages and weather strings is stripped at the co-processor. CJK would need
  an SD-resident glyph file and UTF-8 decoding — evaluated, deferred.
- Menus do not scroll, so the launcher shows the first few entries only.
- WiFi credentials and the bot token are plaintext in `secrets.h` on the ESP32.
