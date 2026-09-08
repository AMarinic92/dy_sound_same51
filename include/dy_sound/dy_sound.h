/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound.h - DY-HV20T / DY-SV MP3 module driver over SERCOM USART
 *
 * Target    : Microchip SAM D5x / E5x (developed on ATSAME51J20A)
 * Toolchain : MPLAB X + MCC Melody, XC32 or ARM-GCC
 *
 * WHY THIS EXISTS
 * ---------------
 * The obvious way to drive one of these modules is to send a play command and
 * wait for the module to acknowledge it. A play command has no acknowledgement.
 *
 * Per the module's manual exactly seven frames have anything in the Return
 * column, and all seven are queries. Every control and setting command - 0x07
 * play included - returns None. Code that waits on the reply to a play decides
 * the cue never started and sends it again. A track that restarts as fast as it
 * can start does not stutter. It goes silent.
 *
 * The fix is not "never ask". It is ask the command that answers:
 *
 *     play  0x07     ->  nothing comes back, ever
 *     wait  gap_ms   ->  let the module seek and start
 *     query 0x0D     ->  AA 0D 02 hi lo sum, a real reply
 *
 * THE RULE THAT KEEPS IT SAFE
 * ---------------------------
 * Only a valid reply naming a DIFFERENT track causes a re-send. Silence never
 * does. A module with its TX unwired, strapped into button mode, or simply mute
 * costs one bounded reply_ms wait per cue and is then left alone. That single
 * condition is the whole difference between this driver and the bug it exists
 * to replace.
 *
 * Replies are located by scanning for the 0xAA start byte and validating the
 * checksum, never by trusting the buffer's alignment. Consuming whatever bytes
 * happen to sit in the FIFO is what makes a desync permanent.
 *
 * REQUIRED MCC CONFIGURATION
 * --------------------------
 *   - A SERCOM in USART mode, 9600 baud, 8 data bits, no parity, 1 stop bit.
 *   - Interrupt / ring-buffer mode OFF. This driver reads the peripheral's
 *     data register itself, so an MCC RX interrupt would drain the reply into
 *     a ring buffer the driver never looks at, and every cue would report "no
 *     reply". If you need that mode, supply cfg.write / cfg.read_byte and
 *     route through the PLIB instead - see the transport override below.
 *
 * The driver does not configure the SERCOM. It binds to one, the same way the
 * companion ws2812-spi driver binds to a SERCOM in SPI master mode.
 *
 * WIRING, AND THE STRAP THAT IS EASY TO MISS
 * ------------------------------------------
 *     module IO0/TX  ->  MCU RX
 *     module IO1/RX  ->  MCU TX
 *     common ground
 *
 * UART mode must be strapped or the module ignores serial entirely - powered,
 * healthy, and completely deaf. CON1->GND, CON2->GND, CON3->3V3, 10 kOhm each
 * (3.3 V is exposed on the module). With DIP switches: CON3 on, CON1/CON2 off.
 * Left floating it sits in button mode. Logic is 3.3 V, so no level shifting.
 *
 * TRACKS ARE BY INDEX, NOT BY NAME
 * --------------------------------
 * 0x07 selects the Nth file in the order it was written to the SD card - not
 * 0007.mp3. Copy files onto an empty card one at a time, in order. Drag them
 * all at once and the numbering silently shifts, and every cue plays the wrong
 * clip.
 * ============================================================================= */

#ifndef DY_SOUND_H
#define DY_SOUND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Pulls in sercom_registers_t. MCC projects place src/config/default on the
 * compiler include path, so this resolves unchanged in any of them. */
#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- Defaults --------------------------------------------------------------
 * Hardware is never the ideal on paper; these are knobs. dy_sound_cfg_init()
 * writes all of them into a cfg, and dy_sound_init() substitutes them for any
 * timing field left at zero.
 * -------------------------------------------------------------------------- */

/** Module attenuator, 0..30. Set once and match levels on the amplifier. */
#define DY_VOLUME_DEFAULT        30u

/** Seek-and-start time the module needs before it will accept the next
 *  command, and how long before the confirming query runs. Raise it if short
 *  clips report a mismatch. */
#define DY_GAP_MS_DEFAULT       200u

/** Bound on the wait for a query's reply. Six bytes at 9600 baud is ~6 ms, so
 *  this is generous - and bounded, which is the point. A mute module costs
 *  this once per cue and nothing else. A valid frame ends the wait early. */
#define DY_REPLY_MS_DEFAULT     120u

/** Re-sends allowed when the module ANSWERS and names a different track.
 *  Silence is never retried, at any setting. */
#define DY_CONFIRM_RETRIES_DEF    1u

/** Commands sent while the module mounts its card are eaten. This is what
 *  otherwise swallows the first cue after power-on. */
#define DY_BOOT_MS_DEFAULT     2000u

/** Settle time after a volume or cycle-mode command. */
#define DY_SETTLE_MS_DEFAULT     50u

/* -- Cycle mode ------------------------------------------------------------
 * From the manual's Loop-mode table. The module's power-on default is
 * DY_CYCLE_ONE_OFF, so an ambient bed left alone plays once and dies. That is
 * what the loop rule is for.
 * -------------------------------------------------------------------------- */
#define DY_CYCLE_ALL           0x00u   /**< whole card in sequence, repeating */
#define DY_CYCLE_LOOP_ONE      0x01u   /**< this track forever - ambient beds  */
#define DY_CYCLE_ONE_OFF       0x02u   /**< once, then stop - power-on default */
#define DY_CYCLE_RANDOM        0x03u   /**< random                             */

/** Not a mode: "leave the mode alone and let the loop rule decide". */
#define DY_CYCLE_KEEP          0xFFu

/* -- Play status, as reported by query 0x01 -------------------------------- */
#define DY_STATUS_STOPPED      0x00u
#define DY_STATUS_PLAYING      0x01u
#define DY_STATUS_PAUSED       0x02u

/* -- Types ------------------------------------------------------------------ */

/**
 * Millisecond time source. Required. Supply xTaskGetTickCount() under FreeRTOS
 * with a 1 kHz tick, or SYSTICK_TimerCounterGet(), or your own millis().
 * Every wait in this driver is bounded by it.
 */
typedef uint32_t (*dy_now_ms_t)(void);

/**
 * Optional sleep. Supplied automatically by the FreeRTOS layer so the driver's
 * waits yield the CPU instead of spinning. If NULL, waits busy-loop on now_ms.
 */
typedef void (*dy_delay_ms_t)(uint32_t ms);

/**
 * Which tracks repeat instead of playing once. NULL means every track plays
 * once and stops. The cycle-mode command is only sent when the answer actually
 * changes, so this costs nothing on a run of ordinary cues.
 */
typedef bool (*dy_loop_rule_t)(uintptr_t context, uint16_t track);

/** Transport override - see cfg.write. */
typedef void (*dy_write_fn_t)(uintptr_t context, const uint8_t *data, uint32_t len);

/** Transport override - see cfg.read_byte. Returns the byte, or -1 if none. */
typedef int  (*dy_read_fn_t)(uintptr_t context);

/**
 * What happened to a cue. Reported instead of logged, so the library carries no
 * stdio dependency and the application decides whether a line is worth
 * printing. Fires from whichever task called play; under the FreeRTOS layer
 * that is the sound task, never an ISR.
 */
typedef enum
{
    /** begin() finished: volume and cycle mode are set. track/reported unused. */
    DY_EVENT_READY = 0,

    /** The module answered and named the track that was asked for.
     *  `reported` is the retry count - normally 0. */
    DY_EVENT_CONFIRMED,

    /** No valid reply inside reply_ms. NOT retried and NOT an error: a module
     *  with TX unwired behaves exactly like this and plays perfectly. */
    DY_EVENT_NO_REPLY,

    /** The module answered and named a different track, and the retries ran
     *  out. `reported` is what it named. Not necessarily a fault - a short clip
     *  can finish before the query lands, and the module then reports whatever
     *  it holds. */
    DY_EVENT_MISMATCH,

    /** A cue was dropped because the queue was full. Fires from the caller's
     *  task, inside dy_sound_rtos_play(). */
    DY_EVENT_DROPPED,

    /** The transmit path stalled - the SERCOM never reported its data register
     *  empty. Almost always a SERCOM that MCC did not enable, or one that is
     *  not in USART mode. */
    DY_EVENT_TX_TIMEOUT
} dy_event_t;

typedef void (*dy_event_cb_t)(uintptr_t context, dy_event_t event,
                              uint16_t track, int reported);

/* Forward declaration - the submit hook takes the device it belongs to. */
struct dy_sound_s;

/**
 * How a cue reaches the module. Installed by the FreeRTOS layer, which points
 * it at the queue; left NULL on bare metal, where a cue is simply executed.
 *
 * This is what lets the bank layer offer one API that works either way. Do not
 * call it directly - it exists so dy_sound_bank_play() does not have to know
 * whether a sound task is running.
 */
typedef bool (*dy_submit_fn_t)(struct dy_sound_s *dev, uint16_t track,
                               uint8_t cycle, bool confirm);

typedef struct
{
    /** SERCOM to drive, e.g. SERCOM5_REGS. Must already be configured and
     *  enabled by MCC as a USART at 9600 8N1. Ignored if write/read_byte are
     *  both supplied. */
    sercom_registers_t *sercom;

    /** Transport override. Leave both NULL to talk to cfg.sercom's data
     *  register directly, which is the normal case. Supply both to route
     *  through something else - MCC's ring-buffer USART PLIB, an RS-485
     *  wrapper, a test harness. Supplying only one is rejected by init. */
    dy_write_fn_t   write;
    dy_read_fn_t    read_byte;
    uintptr_t       io_context;   /**< passed back to write / read_byte */

    /** Required. Bounds every wait in the driver. */
    dy_now_ms_t     now_ms;

    /** Optional. NULL means waits busy-spin on now_ms. The FreeRTOS layer
     *  fills this in for you. */
    dy_delay_ms_t   delay_ms;

    uint8_t         volume;           /**< 0..30, clamped. 0 is silent.       */
    uint8_t         confirm_retries;  /**< 0 disables re-sends entirely.      */
    uint16_t        boot_ms;          /**< 0 -> DY_BOOT_MS_DEFAULT            */
    uint16_t        gap_ms;           /**< 0 -> DY_GAP_MS_DEFAULT             */
    uint16_t        reply_ms;         /**< 0 -> DY_REPLY_MS_DEFAULT           */
    uint16_t        settle_ms;        /**< 0 -> DY_SETTLE_MS_DEFAULT          */

    dy_loop_rule_t  loop_rule;        /**< optional, may be NULL              */
    dy_event_cb_t   on_event;         /**< optional, may be NULL              */
    uintptr_t       context;          /**< passed to loop_rule and on_event   */
} dy_sound_cfg_t;

typedef struct dy_sound_s
{
    sercom_registers_t *sercom;
    dy_write_fn_t   write;
    dy_read_fn_t    read_byte;
    uintptr_t       io_context;
    dy_now_ms_t     now_ms;
    dy_delay_ms_t   delay_ms;
    dy_loop_rule_t  loop_rule;
    dy_event_cb_t   on_event;
    uintptr_t       context;

    uint32_t        init_ms;          /**< when init ran, so begin() can credit
                                       *   boot time already served           */
    uint16_t        boot_ms;
    uint16_t        gap_ms;
    uint16_t        reply_ms;
    uint16_t        settle_ms;
    uint8_t         volume;
    uint8_t         confirm_retries;
    uint8_t         cycle_now;        /**< last mode sent, to avoid resending  */
    bool            ready;            /**< begin() has run                     */

    volatile int32_t confirmed;       /**< last track the module admitted to,
                                       *   or -1 if it did not answer          */

    /* Reserved for the optional bank layer in dy_sound_bank.c. */
    const void     *bank;             /**< dy_bank_t, attached or NULL         */
    uint16_t        selected;         /**< id the room is currently on         */
    uint16_t        rotation;         /**< round-robin cursor for variants     */

    /* Reserved for the optional FreeRTOS layer in dy_sound_freertos.c.
     * Leave these alone if you are not using it. */
    void           *queue;
    void           *task;
    dy_submit_fn_t  submit;
} dy_sound_t;

/* -- Setup ------------------------------------------------------------------ */

/**
 * Fill a cfg with every default. Call this first, then override what you care
 * about. Using a designated initializer instead is fine, but remember that a
 * zeroed `volume` field means silent - the timing fields substitute their
 * defaults when zero, `volume` cannot, because 0 is a legal volume.
 */
void dy_sound_cfg_init(dy_sound_cfg_t *cfg);

/**
 * Bind a module to a SERCOM. Does not touch the wire - no command is sent and
 * nothing is waited on, so this is safe to call from main() before the
 * scheduler starts.
 *
 * Returns false if cfg is missing now_ms, has neither a sercom nor a complete
 * transport override, or supplies only one half of one.
 */
bool dy_sound_init(dy_sound_t *dev, const dy_sound_cfg_t *cfg);

/**
 * Wait out the module's card-mount time, then set volume and cycle mode.
 * Blocks for roughly boot_ms, less whatever has already elapsed since
 * dy_sound_init(). Emits DY_EVENT_READY.
 *
 * Under the FreeRTOS layer the sound task calls this for you.
 */
void dy_sound_begin(dy_sound_t *dev);

/* -- Playback ---------------------------------------------------------------
 * Every call below drives the UART directly and must therefore run on ONE
 * task per module. Two tasks calling these on the same instance will interleave
 * bytes mid-frame. Under the FreeRTOS layer, use the dy_sound_rtos_* calls from
 * other tasks and let the sound task own the port.
 * -------------------------------------------------------------------------- */

/**
 * Play a track, then confirm it - the full sequence described at the top of
 * this file. Costs gap_ms plus however long the module takes to answer, so
 * ~210 ms when it replies and gap_ms + reply_ms (~320 ms) when it does not.
 *
 * Track 0 means "nothing to play" and is rejected.
 *
 * Returns true when the module confirmed the track OR said nothing at all -
 * silence is not a failure, it is an unwired or mute TX line. Returns false
 * only when the module answered, named a different track, and the retries ran
 * out.
 */
bool dy_sound_play(dy_sound_t *dev, uint16_t track);

/** Play without the confirming query. One frame, no waits beyond the write
 *  itself. Use when you know the module's TX is not connected. */
bool dy_sound_play_nowait(dy_sound_t *dev, uint16_t track);

/**
 * Play with the cycle mode stated outright, instead of letting the loop rule
 * decide it. Pass DY_CYCLE_KEEP for `cycle` to fall back to the loop rule -
 * which is exactly what the two calls above do.
 *
 * The mode is still only transmitted when it differs from the last one sent,
 * so passing the same mode on every cue costs nothing. This is what the bank
 * layer uses to drive cycle mode from its table.
 */
bool dy_sound_play_ex(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm);

bool dy_sound_stop(dy_sound_t *dev);
bool dy_sound_pause(dy_sound_t *dev);     /**< toggles pause/resume on the module */
bool dy_sound_next(dy_sound_t *dev);
bool dy_sound_previous(dy_sound_t *dev);

/** Set the module attenuator, 0..30. Values above 30 are clamped. */
bool dy_sound_set_volume(dy_sound_t *dev, uint8_t volume);

/** Set cycle mode explicitly. Normally the loop rule handles this. */
bool dy_sound_set_cycle(dy_sound_t *dev, uint8_t mode);

/* -- Queries ----------------------------------------------------------------
 * These are the only commands the module answers. Each returns false on
 * silence or a frame that failed its checksum, and leaves *out untouched.
 * -------------------------------------------------------------------------- */

bool dy_sound_query_track(dy_sound_t *dev, uint16_t *out);       /**< 0x0D */
bool dy_sound_query_status(dy_sound_t *dev, uint8_t *out);       /**< 0x01 */
bool dy_sound_query_song_count(dy_sound_t *dev, uint16_t *out);  /**< 0x0C */

/**
 * The last track the module admitted to playing, or -1 if it did not answer.
 *
 * Diagnostic only. Do not feed this to a room's status byte - a module that has
 * gone quiet must not make the status read 0. Report the commanded track.
 */
int dy_sound_confirmed(const dy_sound_t *dev);

/* -- Escape hatch ----------------------------------------------------------- */

/**
 * Send one raw frame: AA <cmd> <len> [data...] <sum>. For the handful of module
 * commands this driver does not wrap. len is capped at 4.
 */
bool dy_sound_send_raw(dy_sound_t *dev, uint8_t cmd, const uint8_t *data, uint8_t len);

/**
 * Send a query and wait for its reply, for query commands not wrapped above.
 * want_len is the payload length the module's manual gives for that reply -
 * 1 or 2. Returns false on silence or checksum failure.
 */
bool dy_sound_query_raw(dy_sound_t *dev, uint8_t cmd, uint8_t want_len, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DY_SOUND_H */
