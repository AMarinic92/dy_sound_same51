# Migrating from the Arduino `DY_Sound` library

The protocol is unchanged — same frames, same confirm sequence, same rule about
silence. What changed is everything Arduino was doing for you: the UART, the
logging, and the string of application-side variables that turned a cue code into
a track.

## The short version

| Arduino | Here |
|---|---|
| `DYSound Mast(Serial2, "mast")` | `cfg.sercom = SERCOM5_REGS` |
| `Serial2.begin(9600)` | MCC configures the SERCOM |
| `Mast.begin()` | `dy_sound_init()` then `dy_sound_rtos_start()` |
| `Mast.setLoopRule(mastLoops)` | a `cycle` column in the bank table |
| `Mast.play(14)` | `dy_sound_rtos_play(&mast, 14)` |
| `Mast.confirmed()` | `dy_sound_confirmed(&mast)` |
| `Mast.sendRaw(...)` | `dy_sound_send_raw(...)` |
| `Serial.print` inside the driver | `cfg.on_event` callback |
| `freertos_due_begin/start` | plain `vTaskStartScheduler()` |

## Setting up

Arduino:

```cpp
static DYSound Mast(Serial2, "mast");

void setup() {
  Serial2.begin(9600);
  freertos_due_begin();
  Mast.setLoopRule(mastLoops);
  Mast.begin();
  freertos_due_start();
}
```

Here:

```c
static dy_sound_t mast;

int main(void)
{
    dy_sound_cfg_t      cfg;
    dy_sound_rtos_cfg_t rtos;

    SYS_Initialize(NULL);            /* MCC brings the SERCOM up at 9600 8N1 */

    dy_sound_cfg_init(&cfg);
    cfg.sercom   = SERCOM5_REGS;
    cfg.now_ms   = xTaskGetTickCount;
    cfg.on_event = on_sound_event;
    dy_sound_init(&mast, &cfg);

    dy_sound_bank_attach(&mast, &mast_bank);

    dy_sound_rtos_cfg_init(&rtos);
    rtos.name = "mast";
    dy_sound_rtos_start(&mast, &rtos);

    vTaskStartScheduler();
}
```

Two things that are easy to miss:

- **`cfg.now_ms` is required.** Every wait is bounded by it. `dy_sound_init()`
  returns false without one rather than run something that only looks like it
  works. With `configTICK_RATE_HZ` at 1000, `xTaskGetTickCount` is the answer.
- **Call `dy_sound_cfg_init()` first.** A designated initializer leaves `volume`
  at 0, which is silent. Timing fields substitute their defaults when zero;
  volume cannot, because 0 is a legal volume.

There is no `freertos_due_begin()` / `freertos_due_start()` here — that pair was
a Due port quirk. Use `vTaskStartScheduler()`.

## The `#define` knobs became config fields

The Arduino version took `DY_GAP_MS` and friends as compile-time `#define`s
before the include, so every module on a board shared them. They are now per
instance:

```c
cfg.gap_ms          = 250;
cfg.reply_ms        = 150;
cfg.confirm_retries = 2;
```

The old macro names still exist as the defaults — `DY_GAP_MS_DEFAULT`,
`DY_REPLY_MS_DEFAULT`, `DY_BOOT_MS_DEFAULT`, `DY_CONFIRM_RETRIES_DEF` — and
`DY_VOLUME_DEFAULT` is unchanged at 30.

`boot_ms` now runs from `dy_sound_init()` rather than from the task starting, so
whatever the application spent initialising is credited against it. If you were
carrying a shortened `DY_BOOT_MS` to get the start jingle out sooner, try the
default again first.

## Logging is a callback now

The Arduino version printed to `Serial` through a shared recursive mutex. That
brought `Serial`, a mutex and `configUSE_RECURSIVE_MUTEXES` into a library that
otherwise needs none of them.

Every line it used to print is now an event:

| Old log line | Event |
|---|---|
| `sound: mast ready, volume 30/30` | `DY_EVENT_READY`, `reported` = volume |
| `sound: mast -> track 14  confirmed playing 14` | `DY_EVENT_CONFIRMED`, `reported` = retries |
| `sound: mast -> track 14  (no reply - not retried)` | `DY_EVENT_NO_REPLY` |
| `sound: mast -> track 14  module says 9  (gave up)` | `DY_EVENT_MISMATCH`, `reported` = 9 |

```c
static void on_sound_event(uintptr_t ctx, dy_event_t ev, uint16_t track, int reported)
{
    (void)ctx;
    switch (ev) {
    case DY_EVENT_READY:     printf("sound: mast ready, volume %d/30\n", reported); break;
    case DY_EVENT_CONFIRMED: printf("sound: mast -> %u confirmed\n", track);        break;
    case DY_EVENT_NO_REPLY:  printf("sound: mast -> %u (no reply)\n", track);       break;
    case DY_EVENT_MISMATCH:  printf("sound: mast -> %u, says %d\n", track, reported); break;
    default: break;
    }
}
```

The name that used to be the driver's first constructor argument is now just
whatever you print, plus `rtos.name` for the task. Two events you did not have
before: `DY_EVENT_DROPPED` when the queue is full, and `DY_EVENT_TX_TIMEOUT` when
the SERCOM never reports its data register empty — nearly always a SERCOM MCC did
not enable.

Events fire from task context, never an ISR. If the handler calls `printf`, raise
`rtos.stack_words` past the 256 default.

## The part worth actually rewriting

The library was always "puzzle-agnostic: it moves tracks, it does not decide
them", so a sketch ended up carrying the decision in three places that had to
agree by hand:

```cpp
// 1. cue code -> track index
static uint8_t chestTrackFor(int soundVar2) {
  switch (soundVar2) {
    case 4:           return 1;   // wind
    case 7: case 8:   return 2;   // chest creaking
    case 250:         return 3;   // platform change  (alternates with 251)
    case 251:         return 4;   // platform change
    ...
  }
}

// 2. which tracks repeat, stated somewhere else entirely
static bool mastLoops(uint8_t track) { return track == 6; }

// 3. a latch pair per module, and the edge test
if (SoundVariable2 != LastSoundVariable2) {
  Chest.play(chestTrackFor(SoundVariable2));
  LastSoundVariable2 = SoundVariable2;
}
```

Adding one sound meant editing all three. Worse, (3) is where the original bug
lived — advance the latch in the wrong place and the edge is eaten, so the cue
never plays at all. The sketch carried a comment explaining exactly that.

A bank collapses all three:

```c
enum { SND_WIND = 4, SND_CREAK = 7, SND_PLATFORM = 250, SND_AMBIENT = 252 };

static const dy_track_t chest_tracks[] = {
    DY_TRACK    (SND_WIND,      1, "wind"              ),
    DY_TRACK    (SND_CREAK,     2, "chest creaking"    ),
    DY_TRACK    (SND_PLATFORM,  3, "platform change a" ),
    DY_TRACK    (SND_PLATFORM,  4, "platform change b" ),
    DY_TRACK_BED(SND_AMBIENT,   5, "ambient bed"       ),
};

static const dy_bank_t chest_bank = DY_BANK(chest_tracks);
```

Then the whole of (3) becomes:

```c
dy_sound_bank_select(&chest, SND_PLATFORM);
```

`select()` is edge-triggered internally, so it is safe to call every pass and
there is no latch to place correctly. `LastSoundVariable` and
`LastSoundVariable2` are deleted, not ported.

Three specific things fall out:

- **`mastLoops()` is deleted.** Track 6 was the ambient bed; it becomes one
  `DY_TRACK_BED` line, next to the index it applies to. `dy_sound_bank_attach()`
  clears any loop rule, so do not set both.
- **The 250/251 and 252/253 alternating pairs become one id each**, listed twice.
  The bank hands out variants round-robin, so the application-side timer that
  flipped between the two codes goes away.
- **Two cue codes that mapped to one track** — `case 7: case 8: return 2;` —
  become two lines with the same track. Or one line, if the second code was only
  ever an alias.

The `default: return 0;` arm has no equivalent and does not need one:
`dy_sound_bank_play()` returns false for an id that is not in the table, which is
the useful failure. An unregistered cue code is a typo, not a silence.

### Keeping the switch instead

Nothing forces the bank on you. `dy_sound_rtos_play(&chest, chestTrackFor(code))`
works exactly as before, and `cfg.loop_rule` still takes a predicate — the
signature gained a `uintptr_t context` parameter:

```c
static bool mast_loops(uintptr_t ctx, uint16_t track) { (void)ctx; return track == 6u; }
```

Port straight across first if you like, then move the table over once it runs.

## Smaller differences

- **Track numbers are `uint16_t`.** The Arduino version was `uint8_t`, which
  capped you at 255 even though the module's frame carries 16 bits. Existing
  calls widen silently.
- **`play()` returned `void`; `dy_sound_play()` returns `bool`.** True means
  confirmed *or* silent — silence is not a failure, it is an unwired TX line.
  False means the module answered, named a different track, and the retries ran
  out.
- **A query that answers now returns early**, so a talking module costs about
  10 ms instead of the full `reply_ms`. A silent one still costs `reply_ms`
  exactly once. Per-cue cost drops from ~320 ms to ~210 ms.
- **`sendRaw` caps payloads at 4 bytes** and returns `bool`.
- **New:** `dy_sound_query_status()`, `dy_sound_query_song_count()`,
  `dy_sound_stop/pause/next/previous()`, `dy_sound_rtos_flush()`,
  `dy_sound_rtos_pending()`, and `dy_sound_bank_verify()` — which asks the module
  how many files are on the card and checks the bank fits, catching the
  copied-in-the-wrong-order SD card that otherwise shows up as every cue playing
  the wrong clip.

## What did not change

- The frames. Byte for byte, including the checksums.
- Play, wait, then query `0x0D` — never waiting on an answer to the play.
- **Only a valid reply naming a different track causes a re-send. Silence never
  does.** Still the one condition the whole design rests on.
- Scanning for `0xAA` and validating the checksum rather than trusting alignment.
- One task per module, owning its port outright, with producers that never block.
- The module's power-on cycle mode is still play-once, so a bed still has to be
  declared as one.
