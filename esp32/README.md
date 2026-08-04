# ESP32-S3 WiFi Co-Processor

The STM32F407 has no networking hardware, and putting a TCP/IP stack, TLS, and a
JSON parser on it would eat most of the MCU for one feature. So the whole network
half runs on an **ESP32-S3 acting as a co-processor**: it owns WiFi, HTTPS, NTP
and JSON, and exposes a **line-oriented text protocol** over UART. On the STM32
side that reduces "the internet" to a UART driver.

```
STM32F407  ──UART 115200 8N1──  ESP32-S3  ──WiFi──  Internet

           "WX?"        ──────→            ──→  HTTP GET wttr.in
           "WX Taipei: +33C"  ←──────      ←──
```

**Why this split:** the rate-limit / retry / TLS-handshake timing all lives on the
ESP32, so the STM32 never blocks on the network. `vNetTask` (priority 2) just
asks cheap questions on a timer and drains a ring buffer — see
[`Core/Src/main.c`](../Core/Src/main.c).

## Wiring

Both sides are 3.3 V, so no level shifter is needed. **A common ground is
required** — without it the UART sees garbage or nothing at all.

| STM32F407 | | ESP32-S3 |
|---|---|---|
| `PB10` — USART3_TX | ──→ | `GPIO18` (`STM_RX`) |
| `PB11` — USART3_RX | ←── | `GPIO17` (`STM_TX`) |
| `GND` | ─── | `GND` |

USART3 is on APB1, alternate function `AF7`. Init is hand-written in
`MX_USART3_UART_Init()` / `HAL_UART_MspInit()` — **this project never
regenerates with CubeMX**, which would wipe the hand-written display, touch and
SD pin setup.

## Protocol

Every message is one `\n`-terminated ASCII line. The STM32 sends a command, the
ESP32 replies with exactly one line (except `DL`, below).

| STM32 → ESP32 | ESP32 → STM32 | |
|---|---|---|
| `PING` | `PONG` | link check |
| `WIFI?` | `WIFI OK <ip>` / `WIFI DOWN` | |
| `TIME?` | `TIME HH:MM:SS` / `TIME NONE` | NTP, UTC+8 |
| `WX?` | `WX <text>` / `WX ERR <code>` / `WX DOWN` | wttr.in |
| `MSG?` | `MSG <user>: <text>` / `MSGNONE` | pop one queued Discord message |
| `SEND <text>` | `SENDOK` / `SENDERR` | post to Discord |
| *(unknown)* | `ERR unknown` | |

**ASCII only.** The STM32's bitmap font covers `0x20`–`0x7E`, so `toAscii()`
strips everything else before it goes over the wire (`°` in the weather string
and any CJK in Discord messages). Newlines become spaces so one message stays one
line.

### `DL <url>` — chunked download (ESP32 half only)

The STM32 counterpart (`net_download`) **was removed** — the protocol was
verified working but never got wired to a UI action. This half is kept as
reference; restore the other half with `git show c04cb1b -- Core/Src/main.c`.

```
STM32:  DL <url>
ESP32:  BEGIN
ESP32:  C <len>\n <len raw bytes>      ┐ repeat
STM32:  A                              ┘ (STM32-paced: ESP32 waits for the ack)
ESP32:  C 0                              done
```

Each chunk carries its own length, so the total size never has to be known in
advance — which matters because HTTP/1.0 close-delimited responses have no
`Content-Length`. The per-chunk ack is flow control: the ESP32 cannot outrun the
STM32's 1 KB RX ring buffer while it is busy writing to the SD card.

## Setup

**1. Board support** — Arduino IDE → Boards Manager → **esp32** by Espressif.
Select your ESP32-S3 board.

**2. `USB CDC On Boot` → `Disabled`.** With it enabled, `Serial` goes to the USB
CDC device while the boot ROM log comes out of UART0, so the serial monitor shows
the boot banner and then nothing from your sketch. This one costs an evening if
you don't know it.

**3. Library** — Library Manager → **ArduinoJson** by Benoît Blanchon,
**7.x**. The sketch uses the v7 `JsonDocument` API; 6.x will not compile.

**4. Credentials**

```sh
cd wifi_bridge
cp secrets.h.example secrets.h    # then fill it in
```

`secrets.h` is gitignored. Never put real values in `secrets.h.example`.

**5. Discord bot**

- Developer Portal → New Application → **Bot** → Reset Token → copy into `secrets.h`
- **Bot → MESSAGE CONTENT INTENT must be ON.** Without it `content` comes back as
  an empty string with **no error** — the poll succeeds, messages just silently
  have no text. Easily the worst thing to debug here.
- OAuth2 → URL Generator → scope `bot`, permissions **View Channels + Send
  Messages + Read Message History** → open the URL to invite it to your server
- Discord Settings → Advanced → Developer Mode ON → right-click the channel →
  Copy Channel ID → `DC_CHANNEL`

| HTTP code | meaning |
|---|---|
| 401 | bad token |
| 403 | bot not in the server, or missing permission |
| 404 | wrong channel ID |

**6. Flash it**, open the serial monitor at 115200. You should see `WiFi OK, IP =
...`. You can then **type into the serial monitor to post straight to Discord** —
that debug path in `loop()` lets you verify the ESP32↔Discord half on its own,
before involving the STM32 at all. Splitting bring-up like that is worth doing.

## Design notes

- **The ESP32 polls Discord itself** (every 3 s) into its own 8-slot queue; the
  STM32 only does a cheap `MSG?`. Network cadence stays on the network side.
- **First poll uses `?limit=1`** purely to establish a baseline snowflake ID —
  otherwise the whole channel history dumps out on boot.
- **Messages from bots are skipped**, or the bot reads back its own posts 3
  seconds later and talks to itself forever.
- **REST polling, not the Gateway WebSocket.** The Gateway is heavy for an ESP32
  and 3-second latency is invisible in a chat app.
- `setInsecure()` skips certificate validation. Fine for a personal project;
  it does mean the TLS connection is not authenticated.
