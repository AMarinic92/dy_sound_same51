# dy_sound_same51

DY-HV20T / DY-SV MP3 module driver for Microchip SAM D5x / E5x, using a SERCOM
in USART mode. Developed on an ATSAME51J20A.

Ported from the Arduino `DY_Sound` library — see [MIGRATION.md](MIGRATION.md) if
you are moving a sketch across.

- No hard-coded SERCOM — it is a runtime parameter, like `ws2812-spi`
- Sounds declared in one table: index, repeat behaviour and label on one line
- Edge-triggered `select()`, so the `LastSoundVariable` latch leaves your code
- Optional FreeRTOS task layer; producers post a cue and never block
- No stdio, no `malloc`, no `String` — diagnostics come out as a callback

## The bug this exists to not have

The obvious way to drive one of these modules is to send a play command and wait
for the module to acknowledge it. **A play command has no acknowledgement.**

Per the module's manual exactly seven frames have anything in the Return column,
and all seven are queries. Every control and setting command — `0x07` play
included — returns None:

| Query | Frame | Reply |
|---|---|---|
| play status | `AA 01 00 AB` | `AA 01 01 <status> SM` |
| current online drive | `AA 09 00 B3` | `AA 09 01 <drive> SM` |
| current play drive | `AA 0A 00 B4` | `AA 0A 01 <drive> SM` |
| number of songs | `AA 0C 00 B6` | `AA 0C 02 <hi> <lo> SM` |
| **current song** | `AA 0D 00 B7` | `AA 0D 02 <hi> <lo> SM` |
| folder directory song | `AA 11 00 BB` | `AA 11 02 <hi> <lo> SM` |
| folder number of songs | `AA 12 00 BC` | `AA 12 02 <hi> <lo> SM` |

Code that waits on the reply to a play decides the cue never started and sends it
again. A track that restarts as fast as it can start does not stutter — it goes
silent.

The fix is not "never ask". It is **ask the command that answers**:

```
play  0x07     ->  nothing comes back, ever
wait  gap_ms   ->  let the module seek and start
query 0x0D     ->  AA 0D 02 hi lo sum, a real reply
```

### The rule that keeps it safe

**Only a valid reply naming a different track causes a re-send. Silence never
does.**

A module with its TX unwired, strapped into button mode, or simply mute costs one
bounded `reply_ms` wait per cue and is then left alone. The room keeps running
either way.

Replies are found by scanning for `0xAA` and validating the checksum, never by
trusting the buffer's alignment — consuming whatever bytes happen to sit in the
FIFO is what makes a desync permanent.

## Requirements

Configure in MCC Melody:

| Peripheral | Setting |
|---|---|
| SERCOM (any) | USART, **9600** baud, 8 data bits, no parity, 1 stop bit |

**Leave interrupt / ring-buffer mode off.** The driver reads the peripheral's
data register itself. An MCC RX interrupt would drain the module's reply into a
ring buffer the driver never looks at, and every cue would report `no reply`. If
you need that mode, use the [transport override](#a-sercom-you-do-not-own).

The driver does not configure the SERCOM, it binds to one — the same division of
labour as the companion [`ws2812-spi`](https://github.com/AMarinic92/ws2812-spi)
driver.

### Wiring, and the strap that is easy to miss

```
module IO0/TX  ->  MCU RX
module IO1/RX  ->  MCU TX
common ground
```

**UART mode must be strapped or the module ignores serial entirely** — powered,
healthy, and completely deaf. `CON1`→GND, `CON2`→GND, `CON3`→3V3, 10 kΩ each
(3.3 V is exposed on the module). With DIP switches: `CON3` on, `CON1`/`CON2`
off. Left floating it sits in button mode.

Logic is 3.3 V, so no level shifting either way.

### Tracks are by index, not by name

`0x07` selects the **Nth file in the order it was written to the SD card** — not
`0007.mp3`. Copy files onto an empty card one at a time, in order. Drag them all
at once and the numbering silently shifts, and every cue plays the wrong clip.

`dy_sound_bank_verify()` catches the common half of this at startup.

## Adding it to an MPLAB X project

```sh
git submodule add https://github.com/AMarinic92/dy_sound_same51.git lib/dy_sound_same51
```

Then add `lib/dy_sound_same51/include` to `compiler.extra-include-directories`
in your project's `.mplab.json` and regenerate.

If your project's file set globs the repository — the default for newer MPLAB
projects — the sources under `lib/dy_sound_same51/src` are picked up
automatically and compiled with the project's own toolchain flags. Otherwise add
them to `user.cmake`:

```cmake
set(DY_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/dy_sound_same51")

target_sources(${YOUR_OBJ_TARGET} PRIVATE
    "${DY_DIR}/src/dy_sound.c"
    "${DY_DIR}/src/dy_sound_bank.c"
    "${DY_DIR}/src/dy_sound_freertos.c")

target_include_directories(${YOUR_OBJ_TARGET} PRIVATE "${DY_DIR}/include")
target_compile_definitions(${YOUR_OBJ_TARGET} PRIVATE DY_SOUND_ENABLE_FREERTOS)
```

Do not link the executable target by name — it carries a random suffix that
changes when the project is regenerated. The object-library target is stable.

## Defining sounds

A room's sounds go in one table. Index, repeat behaviour and a label sit on one
line, and nothing else in the application needs to know any of them.

```c
#include "dy_sound/dy_sound_bank.h"

enum { SND_WIND = 1, SND_PLATFORM, SND_AMBIENT, SND_CANNON, SND_HINT };

static const dy_track_t chest_tracks[] = {
    DY_TRACK    (SND_WIND,      1, "wind"              ),
    DY_TRACK    (SND_PLATFORM,  3, "platform change a" ),
    DY_TRACK    (SND_PLATFORM,  4, "platform change b" ),
    DY_TRACK_BED(SND_AMBIENT,   5, "ambient bed"       ),
    DY_TRACK    (SND_CANNON,    8, "cannon fire"       ),
    DY_TRACK    (SND_HINT,      9, "generic hint"      ),
    DY_TRACK    (SND_HINT,     11, "fancy a wee hint"  ),
};

static const dy_bank_t chest_bank = DY_BANK(chest_tracks);
```

| Macro | Cycle mode | For |
|---|---|---|
| `DY_TRACK` | play once, stop | almost everything |
| `DY_TRACK_BED` | repeat until something else plays | ambient beds |
| `DY_TRACK_MODE` | whatever you name | `DY_CYCLE_ALL`, `DY_CYCLE_RANDOM` |

The module's power-on default is play-once, so a bed declared with `DY_TRACK`
plays once and dies. That is the whole reason the column exists.

**Repeat an id to declare variants.** They are handed out round-robin, so the
ambient that alternates between two beds, or the hint line that should not repeat
verbatim, is one extra line and no application state.

Attach the bank after init:

```c
dy_sound_bank_attach(&chest, &chest_bank);
```

## Switching sounds

```c
dy_sound_bank_select(&chest, SND_AMBIENT);   /* plays */
dy_sound_bank_select(&chest, SND_AMBIENT);   /* does nothing */
dy_sound_bank_select(&chest, SND_CANNON);    /* plays */
```

`select()` is edge-triggered: it plays only when the id differs from the last one
selected, so it is safe to call every pass of a loop. That is what removes the
`LastSoundVariable` latch — and with it the class of bug where the latch advances
in the wrong place, eats the edge, and the cue never plays at all.

| Call | Behaviour |
|---|---|
| `dy_sound_bank_select(dev, id)` | Play on a change only. The one you want. |
| `dy_sound_bank_play(dev, id)` | Play now, change or not. Advances variants. |
| `dy_sound_bank_replay(dev)` | The selected id again, from the top. |
| `dy_sound_bank_reset(dev)` | Forget the selection. Use on a room reset. |

Selecting `DY_SOUND_NONE` records that nothing is cued and sends nothing — it
does **not** stop what is playing. A cue that has already started should finish;
call `dy_sound_stop()` when you actually mean stop.

`dy_sound_bank_play()` returns false for an id that is not in the table, which is
the useful failure: an unregistered cue code is a typo, not a silence.

## Usage

```c
#include "dy_sound/dy_sound.h"
#include "dy_sound/dy_sound_bank.h"
#include "dy_sound/dy_sound_freertos.h"

static dy_sound_t chest;

static void on_sound_event(uintptr_t ctx, dy_event_t ev, uint16_t track, int reported)
{
    (void)ctx;
    switch (ev) {
    case DY_EVENT_READY:     printf("sound: ready, volume %d/30\n", reported);        break;
    case DY_EVENT_CONFIRMED: printf("sound: %u confirmed\n", track);                  break;
    case DY_EVENT_NO_REPLY:  printf("sound: %u, no reply - not retried\n", track);    break;
    case DY_EVENT_MISMATCH:  printf("sound: %u, module says %d\n", track, reported);  break;
    case DY_EVENT_DROPPED:   printf("sound: %u dropped, queue full\n", track);        break;
    default:                 printf("sound: TX stalled\n");                           break;
    }
}

int main(void)
{
    dy_sound_cfg_t      cfg;
    dy_sound_rtos_cfg_t rtos;

    SYS_Initialize(NULL);

    dy_sound_cfg_init(&cfg);          /* every default, then override */
    cfg.sercom   = SERCOM5_REGS;
    cfg.now_ms   = xTaskGetTickCount;
    cfg.on_event = on_sound_event;

    dy_sound_init(&chest, &cfg);
    dy_sound_bank_attach(&chest, &chest_bank);

    dy_sound_rtos_cfg_init(&rtos);
    rtos.name = "sound";
    dy_sound_rtos_start(&chest, &rtos);   /* before the scheduler */

    vTaskStartScheduler();
}
```

Then, from any task:

```c
dy_sound_bank_select(&chest, SND_CANNON);
```

Always call `dy_sound_cfg_init()` first. A designated initializer works too, but
a zeroed `volume` field means **silent** — the timing fields substitute their
defaults when zero, volume cannot, because 0 is a legal volume.

### FreeRTOS

Define `DY_SOUND_ENABLE_FREERTOS`; without it `src/dy_sound_freertos.c` compiles
to nothing (verified: 0 bytes of text), so the file is safe to leave in a globbed
file set on a bare-metal project.

One task per module. The task owns its SERCOM outright, so there are no locks on
the hardware, and because it is the only thing that ever writes to the port two
frames cannot interleave on the wire.

Producers never block. Every `dy_sound_rtos_*` call and every bank call posts one
cue and returns; past `queue_len` the newest is dropped and `DY_EVENT_DROPPED`
fires, rather than stalling a prop.

Needs only `configSUPPORT_DYNAMIC_ALLOCATION` and `INCLUDE_vTaskDelay`. No
mutexes, no task notifications, no software timers — so nothing here conflicts
with a notification index another driver is using, and
`configUSE_RECURSIVE_MUTEXES` may stay 0.

The bank works identically with or without the task. It resolves the id against
the table — pure lookup, no I/O — and hands the result to whichever path the
device is set up for. Your calling code reads the same either way.

To use the direct API from a task of your own instead, skip `rtos_start` and
supply the delay hook yourself:

```c
cfg.now_ms   = xTaskGetTickCount;
cfg.delay_ms = dy_sound_rtos_delay_ms;
```

You give up the non-blocking producer API and take the ~210 ms per cue yourself.

### Bare metal

Supply a millisecond source and nothing else:

```c
cfg.now_ms = SYSTICK_TimerCounterGet;   /* or your own millis() */
```

`now_ms` is required — every wait in the driver is bounded by it, including the
transmit timeout, so `dy_sound_init()` refuses a config without one rather than
run something that only looks like it works. Without a `delay_ms`, waits
busy-spin.

## Fire and forget

A confirmed cue occupies the sound task for `gap_ms` plus a reply window — about
210 ms when the module answers, 320 ms when it does not. The caller never waits,
but the *next* cue does, so a burst of stingers plays back-to-back at three a
second at best.

When nobody is going to look at the answer, skip it:

| | Sound task spends | Snapshot afterwards |
|---|---|---|
| `dy_sound_rtos_play(&dev, 14)` | ~210–320 ms | the module's answer, or -1 |
| `dy_sound_rtos_play_nowait(&dev, 14)` | ~6 ms — one frame | -1, "nobody asked" |
| `dy_sound_bank_play(&dev, SND_CANNON)` | ~210–320 ms | as above |
| `dy_sound_bank_play_nowait(&dev, SND_CANNON)` | ~6 ms | -1, "nobody asked" |

`_nowait` is not a worse version of the other one. It is the correct version
when the module's `IO0/TX` is not wired back to the MCU, because then there is
no answer to be had and the confirming query is 120 ms of waiting for silence,
per cue, forever.

## Asking what is playing

The module has a BUSY pin, and it answers a different question from the UART:
the pin says **that** something is playing, the UART says **what**. Watching the
pin is the application's job — it is the only thing that knows which MCU pin the
module landed on — but the moment the edge fires, the next thought is "so what is
it playing?", and that cannot be asked from an interrupt. A query takes up to
`reply_ms` and drives a UART that belongs to the sound task.

So the handler does not ask. It raises a flag:

```c
void BUSY_EdgeHandler(uintptr_t ctx)        // EIC callback
{
    (void)ctx;
    dy_sound_rtos_ask_from_isr(&module, DY_ASK_BOTH, NULL);
}
```

The sound task spends it on its own time, and the answer waits in a snapshot:

```c
dy_status_t snap;

dy_sound_status(&module, &snap);            // any task, or an ISR; never blocks

if (snap.playing)
{
    printf("track %ld, asked %lu ms ago\n",
           (long)snap.track,
           (unsigned long)(xTaskGetTickCount() - snap.asked_ms));
}
```

| Field | |
|---|---|
| `track` | what the last query named, **-1 if it went unanswered**. The one to act on. |
| `last_track` | the last track the module ever named. Survives silence. Diagnostic. |
| `status` | `DY_STATUS_PLAYING` / `PAUSED` / `STOPPED`, or `DY_STATUS_UNKNOWN` |
| `playing` | `status == DY_STATUS_PLAYING`, for readability |
| `answered` | the most recent query got a valid reply |
| `asked_ms` | `now_ms` when that query went out — subtract from now to age it |
| `queries` / `answers` | totals. Equal after a request means the module replied. |

`track` and `last_track` are two fields because the difference between them is
the trap. A query the module ignored must not leave the old answer sitting in
one variable looking current — that is how a module that went quiet five minutes
ago reads as "still playing track 6". `track` goes to -1 the moment a query is
ignored, and to -1 again the moment a new cue, stop or pause changes what is
playing. `last_track` never expires. Act on `track`, explain with `last_track`.

`DY_ASK_TRACK` is `0x0D`, `DY_ASK_STATUS` is `0x01`, `DY_ASK_BOTH` is both.
Asking for both costs two reply windows on a silent module and about 20 ms on a
talking one.

| Call | Context | Does |
|---|---|---|
| `dy_sound_rtos_ask_from_isr(dev, what, woken)` | ISR | raise + wake the task |
| `dy_sound_rtos_ask(dev, what)` | any task | raise + wake the task |
| `dy_sound_ask(dev, what)` | anywhere | raise only, no RTOS call at all |
| `dy_sound_service(dev)` | the task owning the port | spend whatever is raised |
| `dy_sound_refresh(dev, what)` | the task owning the port | ask right now, blocking |
| `dy_sound_status(dev, &snap)` | anywhere, ISR included | read, never transmits |

Some properties worth knowing:

- **Edges coalesce.** The bits are OR-ed, so a bouncing line costs one query,
  not one per edge. The wake-up carries no payload precisely so that a full
  queue cannot turn a chattering pin into eight dropped cues.
- **A cue answers its own request.** A request already pending when a cue's
  confirming `0x0D` goes out is served by it — the driver does not ask twice for
  the same thing. One raised *after* that frame is on the wire survives, because
  it is about a state newer than the answer coming back.
- **A backed-up queue delays it, never loses it.** The task calls
  `dy_sound_service()` after *every* item it pulls off the queue, so a request
  raised during a burst is served after the next cue.
- **Nothing polls.** There is no idle wake period and no timer — the task sleeps
  on its queue and the request itself is what wakes it.
- **`dy_sound_status()` is a seqlock read**, bounded to four attempts because
  the reader may be the interrupt that preempted the writer. It returns `false`
  on a torn read and still fills `*out`: fine for reading one field, not for
  comparing `queries` against `answers`.

On bare metal there is no task to wake, so call `dy_sound_service()` from the
superloop — it costs a flag test when nothing is pending — or skip the flag and
call `dy_sound_refresh()` directly.

## Diagnostics

The library has no stdio dependency. It reports through `cfg.on_event` and lets
the application decide whether a line is worth printing.

| Event | Meaning |
|---|---|
| `DY_EVENT_READY` | `begin()` done, volume and cycle set. `reported` is the volume. |
| `DY_EVENT_CONFIRMED` | Module named the track asked for. `reported` is the retry count. |
| `DY_EVENT_NO_REPLY` | Nothing inside `reply_ms`. **Not** retried, **not** an error. |
| `DY_EVENT_MISMATCH` | Module named a different track and the retries ran out. |
| `DY_EVENT_DROPPED` | Queue was full. Fires in the caller's task. |
| `DY_EVENT_TX_TIMEOUT` | The SERCOM never reported its data register empty. |
| `DY_EVENT_TRACK` | Answer to `dy_sound_rtos_query_track()`. `track` is what the module is playing. |

`dy_sound_rtos_query_track()` exists for a BUSY-line interrupt: the edge says a
sound started, the query asks which. It is queued like a cue, so it runs on the
sound task and never collides with a play on the wire. Silence comes back as
`DY_EVENT_NO_REPLY` with track 0.

Events fire from task context, never an ISR, so `printf` is fine if your config
allows it — raise `stack_words` if you do.

`dy_sound_confirmed()` is diagnostic. **Do not feed it to a room's status byte** —
a module that has gone quiet must not make the status read 0. Report the
commanded track.

`DY_EVENT_MISMATCH` is not automatically a fault: a short clip can finish before
the query lands, and the module then reports whatever it holds. It is reported
once and never chased.

## A SERCOM you do not own

If MCC has the SERCOM in ring-buffer mode, or you are going through an RS-485
wrapper, route the bytes yourself:

```c
static void plib_write(uintptr_t ctx, const uint8_t *data, uint32_t len)
{ (void)ctx; SERCOM5_USART_Write((void *)data, len); }

static int plib_read(uintptr_t ctx)
{ (void)ctx; return SERCOM5_USART_ReceiverIsReady() ? SERCOM5_USART_ReadByte() : -1; }

cfg.write     = plib_write;
cfg.read_byte = plib_read;
```

Supply both or neither — half an override is rejected by `init`, because writing
through the PLIB while reading the raw register fights whatever owns the other
half.

## Knobs

Set them on the cfg after `dy_sound_cfg_init()`. Any timing field left at zero
takes its default.

| Field | Default | Turn it when |
|---|---|---|
| `volume` | 30 | 0..30, the module's own attenuator. Clamped. |
| `gap_ms` | 200 | Rapid cues drop, or short clips report a mismatch because they finished before the query landed. Seek-and-start time; an estimate. |
| `reply_ms` | 120 | Confirmations read `no reply` on a module you know is talking. Six bytes at 9600 is ~6 ms, so the default is generous. A valid frame ends the wait early. |
| `confirm_retries` | 1 | Cues genuinely fail to land and one re-send is not enough. Raising it multiplies worst-case time per cue. |
| `boot_ms` | 2000 | The first cue after power-on gets eaten while the module mounts its card. Measured from `dy_sound_init()`, so time already spent initialising is credited. |
| `settle_ms` | 50 | After a volume or cycle command. |
| `queue_len` | 8 | Cues pile up. Past this the newest is dropped. |
| `stack_words` | 256 | Raise it if `on_event` calls `printf`. |

A confirmed cue costs `gap_ms` plus the module's reply — about 210 ms. A silent
one costs `gap_ms + reply_ms`, ~320 ms, once, and is not retried. All of it
inside the sound task.

## Cost

| | text |
|---|---|
| `dy_sound.c` | 2268 B |
| `dy_sound_bank.c` | 664 B |
| `dy_sound_freertos.c` | 1168 B, or 0 without the define |

XC32 4.60 at `-O1`, no static RAM beyond the `dy_sound_t` you declare. Set
`DY_SOUND_LABELS=0` to drop the bank's label strings.

## Limits

- **One instance per SERCOM.** The task owns its port outright, which is what
  makes it safe without locks. Two instances on one port would race.
- Bank lookups are a linear scan. A room's vocabulary is tens of entries — a
  handful of 16-bit compares against a table in flash, immaterial next to the
  200 ms the cue itself costs.
- Track indices are 16-bit, so the module's full range is available. The Arduino
  version was 8-bit.
- Payloads for `dy_sound_send_raw()` are capped at 4 bytes, which covers every
  documented command.

## License

[Mozilla Public License 2.0](LICENSE).

File-level copyleft: if you modify these files, those modifications have to stay
open. Linking the library into a larger, closed-source firmware image is fine,
and the rest of your project is unaffected.
