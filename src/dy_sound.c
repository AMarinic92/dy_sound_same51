/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound.c - DY-HV20T / DY-SV MP3 module driver over SERCOM USART
 * See include/dy_sound/dy_sound.h for the protocol and the confirm rule.
 * ============================================================================= */

#include "dy_sound/dy_sound.h"

/* -- Module command set ----------------------------------------------------
 * Control and setting commands. None of these are answered.
 * -------------------------------------------------------------------------- */
#define DY_CMD_PAUSE            0x03u
#define DY_CMD_STOP             0x04u
#define DY_CMD_PREVIOUS         0x05u
#define DY_CMD_NEXT             0x06u
#define DY_CMD_SELECT_PLAY      0x07u
#define DY_CMD_SET_VOLUME       0x13u
#define DY_CMD_SET_CYCLE        0x18u

/* Queries. These are answered. */
#define DY_CMD_Q_STATUS         0x01u
#define DY_CMD_Q_SONG_COUNT     0x0Cu
#define DY_CMD_Q_CURRENT_SONG   0x0Du

#define DY_FRAME_START          0xAAu

/** Largest payload dy_sound_send_raw() will accept. */
#define DY_MAX_PAYLOAD             4u

/** RX scratch. A reply is at most 6 bytes; the slack absorbs whatever noise
 *  preceded it, which is exactly what the start-byte scan is there to skip. */
#define DY_RX_SCRATCH             24u

/** Bound on the drain loop, so a SERCOM stuck with RXC asserted cannot hang. */
#define DY_DRAIN_LIMIT            64u

/** Bound on the per-byte wait for the data register to empty. One byte at 9600
 *  baud is ~1.04 ms; anything past this means the SERCOM is not running. */
#define DY_TX_TIMEOUT_MS          20u

/** Tries a snapshot reader gets before it reports a torn read. Bounded because
 *  the reader may BE the interrupt that preempted the writer, in which case the
 *  writer cannot possibly finish until we return - spinning would be a hang. */
#define DY_STATUS_READ_TRIES       4u

/* Which snapshot field a query's answer belongs in. */
#define DY_REC_NONE                0u
#define DY_REC_TRACK               1u
#define DY_REC_STATUS              2u

/* -- Concurrency primitives -------------------------------------------------
 * One core, so a compiler barrier is the whole of what the seqlock needs: the
 * only other observer is an interrupt or a preempting task on this same CPU,
 * and those see this core's accesses in program order. What must not happen is
 * the COMPILER hoisting a field write across the sequence bump, which is
 * exactly what this forbids.
 * -------------------------------------------------------------------------- */
#if defined(__GNUC__)
#define DY_BARRIER()    __asm__ volatile ("" ::: "memory")
#else
#define DY_BARRIER()    do { } while (0)
#endif

/* Masking PRIMASK rather than taking a FreeRTOS critical section keeps this
 * file free of the RTOS, and the guarded region is two byte accesses - far
 * shorter than the tick handler it briefly delays. PRIMASK is saved and
 * restored rather than blindly re-enabled, so calling this from inside an
 * already-masked region does not silently open the door. */
static uint32_t dy_irq_save(void)
{
    uint32_t saved = __get_PRIMASK();

    __disable_irq();
    return saved;
}

static void dy_irq_restore(uint32_t saved)
{
    __set_PRIMASK(saved);
}

/* -- Time ------------------------------------------------------------------- */

static uint32_t dy_now(const dy_sound_t *dev)
{
    /* now_ms is checked for NULL in init, so every device that exists has one. */
    return dev->now_ms();
}

/* Unsigned subtraction throughout, so a counter rollover cannot end a wait
 * early or stretch it to nearly forever. */
static bool dy_elapsed(const dy_sound_t *dev, uint32_t start, uint32_t ms)
{
    return (uint32_t)(dy_now(dev) - start) >= ms;
}

static void dy_wait(dy_sound_t *dev, uint32_t ms)
{
    uint32_t start;

    if (ms == 0u)
    {
        return;
    }

    if (dev->delay_ms != NULL)
    {
        dev->delay_ms(ms);
        return;
    }

    start = dy_now(dev);

    while (!dy_elapsed(dev, start, ms))
    {
        /* spin - documented as the cost of not supplying a delay hook */
    }
}

static void dy_emit(const dy_sound_t *dev, dy_event_t event, uint16_t track, int reported)
{
    if (dev->on_event != NULL)
    {
        dev->on_event(dev->context, event, track, reported);
    }
}

/* -- The snapshot -----------------------------------------------------------
 * Every query in the driver reports its outcome here, so there is exactly one
 * place that decides what "what is playing" means, and one place that can be
 * read from an ISR without a lock.
 * -------------------------------------------------------------------------- */

static void dy_record(dy_sound_t *dev, uint8_t kind, bool answered,
                      uint16_t value, uint32_t asked_ms)
{
    /* Odd for the duration. A reader that sees the same even value either side
     * of its copy knows the copy did not straddle this. */
    dev->status_seq++;
    DY_BARRIER();

    dev->queries++;
    dev->asked_ms = asked_ms;
    dev->answered = answered;

    if (answered)
    {
        dev->answers++;
    }

    if (kind == DY_REC_TRACK)
    {
        /* -1 on silence, deliberately. Leaving the previous answer in place is
         * what makes a module that stopped answering five minutes ago read as
         * if it had just confirmed. Anyone who wants that value can have it
         * from last_track, where it is labelled as old. */
        dev->confirmed = answered ? (int32_t)value : -1;

        if (answered)
        {
            dev->last_track = (int32_t)value;
        }
    }
    else if ((kind == DY_REC_STATUS) && answered)
    {
        dev->status = (uint8_t)value;
    }
    else
    {
        /* DY_REC_NONE: song count, or a raw query. It still counts and still
         * dates the snapshot - it simply names no field. */
    }

    DY_BARRIER();
    dev->status_seq++;
}

/* A query serves the matching request no matter who raised it, including the
 * 0x0D a cue sends to confirm itself - that is what keeps a BUSY edge during a
 * cue from costing a second query for an answer already in hand.
 *
 * Cleared BEFORE the query goes out, never after. An edge that arrives while we
 * are on the wire is about a state newer than the answer coming back, so it has
 * to survive into the next round; clearing afterwards would drop exactly the
 * request that most needed asking. */
static void dy_clear_ask(dy_sound_t *dev, uint8_t bit)
{
    uint32_t saved = dy_irq_save();

    dev->ask_flags = (uint8_t)(dev->ask_flags & (uint8_t)(~bit));
    dy_irq_restore(saved);
}

/* A play changes what is playing, which makes the last answer about what is
 * playing wrong. Nothing has been asked since, so the snapshot says so instead
 * of holding out the previous track as current - the whole point of `track`
 * being allowed to read -1.
 *
 * last_track and the counters are untouched: they describe the module's
 * history, not the present, and this is the present changing. */
static void dy_invalidate(dy_sound_t *dev)
{
    dev->status_seq++;
    DY_BARRIER();

    dev->confirmed = -1;
    dev->status    = (uint8_t)DY_STATUS_UNKNOWN;
    dev->answered  = false;

    DY_BARRIER();
    dev->status_seq++;
}

/* Read the pending bits and clear them in one go. An ISR that raises a bit
 * between the read and the clear would otherwise have its request thrown away
 * by the clear - the one race in the whole mechanism, and the reason this is
 * not two plain statements. */
static uint8_t dy_take_ask(dy_sound_t *dev)
{
    uint32_t saved = dy_irq_save();
    uint8_t  what  = dev->ask_flags;

    dev->ask_flags = 0u;
    dy_irq_restore(saved);

    return what;
}

/* -- Byte level I/O ---------------------------------------------------------
 * Direct register access by default. The SERCOM is configured and enabled by
 * MCC; all this touches is DATA, INTFLAG and STATUS, which is the same division
 * of labour the ws2812-spi driver uses for SPI.
 * -------------------------------------------------------------------------- */

static bool dy_tx(dy_sound_t *dev, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint32_t start;

    if (dev->write != NULL)
    {
        dev->write(dev->io_context, data, len);
        return true;
    }

    for (i = 0u; i < len; i++)
    {
        start = dy_now(dev);

        while ((dev->sercom->USART_INT.SERCOM_INTFLAG
                & SERCOM_USART_INT_INTFLAG_DRE_Msk) == 0u)
        {
            /* A SERCOM that MCC never enabled never raises DRE. Without this
             * bound the whole sound task would wedge on a config mistake. */
            if (dy_elapsed(dev, start, DY_TX_TIMEOUT_MS))
            {
                dy_emit(dev, DY_EVENT_TX_TIMEOUT, 0u, -1);
                return false;
            }
        }

        dev->sercom->USART_INT.SERCOM_DATA = (uint32_t)data[i];
    }

    return true;
}

static int dy_rx(dy_sound_t *dev)
{
    uint8_t flags;

    if (dev->read_byte != NULL)
    {
        return dev->read_byte(dev->io_context);
    }

    flags = (uint8_t)dev->sercom->USART_INT.SERCOM_INTFLAG;

    /* Clear the sticky error bits before reading. A latched BUFOVF otherwise
     * survives into later cues. The bytes themselves are not discarded on
     * error - a corrupted one simply fails the checksum, which is the whole
     * reason replies are validated rather than trusted. */
    if ((flags & SERCOM_USART_INT_INTFLAG_ERROR_Msk) != 0u)
    {
        dev->sercom->USART_INT.SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_ERROR_Msk;
        dev->sercom->USART_INT.SERCOM_STATUS  = (uint16_t)(SERCOM_USART_INT_STATUS_PERR_Msk
                                                         | SERCOM_USART_INT_STATUS_FERR_Msk
                                                         | SERCOM_USART_INT_STATUS_BUFOVF_Msk);
    }

    if ((flags & SERCOM_USART_INT_INTFLAG_RXC_Msk) == 0u)
    {
        return -1;
    }

    /* Reading DATA is what clears RXC. */
    return (int)(dev->sercom->USART_INT.SERCOM_DATA & 0xFFu);
}

/* Drop whatever is already in the receiver, so a stale frame cannot be mistaken
 * for the answer to the query about to go out. */
static void dy_drain(dy_sound_t *dev)
{
    uint32_t guard;

    for (guard = 0u; guard < DY_DRAIN_LIMIT; guard++)
    {
        if (dy_rx(dev) < 0)
        {
            return;
        }
    }
}

/* -- Framing ----------------------------------------------------------------
 * AA <cmd> <len> [data...] <sum>, where sum is every preceding byte added up
 * and truncated to 8 bits.
 * -------------------------------------------------------------------------- */

static bool dy_send(dy_sound_t *dev, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t frame[3u + DY_MAX_PAYLOAD + 1u];
    uint8_t sum = 0u;
    uint8_t n   = 0u;
    uint8_t i;

    if (len > DY_MAX_PAYLOAD)
    {
        return false;
    }

    frame[n++] = DY_FRAME_START;
    frame[n++] = cmd;
    frame[n++] = len;

    for (i = 0u; i < len; i++)
    {
        frame[n++] = data[i];       /* len 0 -> data is never dereferenced */
    }

    for (i = 0u; i < n; i++)
    {
        sum = (uint8_t)(sum + frame[i]);
    }

    frame[n++] = sum;

    return dy_tx(dev, frame, (uint32_t)n);
}

/*
 * Send a query and wait for its reply.
 *
 * The reply is found by scanning for the start byte and checking the sum, never
 * by assuming the first byte read is the first byte of the frame. Trusting
 * alignment is what turns one stray byte into a permanent desync.
 *
 * Returns as soon as a valid frame lands rather than always burning the full
 * window, so a module that answers costs ~10 ms instead of reply_ms. A module
 * that says nothing costs reply_ms exactly once and is not asked twice.
 */
static bool dy_query(dy_sound_t *dev, uint8_t cmd, uint8_t want_len, uint16_t *out)
{
    uint8_t  buf[DY_RX_SCRATCH];
    uint8_t  n = 0u;
    uint8_t  i;
    uint8_t  need;
    uint32_t start;
    int      byte;

    if ((want_len == 0u) || (want_len > 2u))
    {
        return false;
    }

    need = (uint8_t)(3u + want_len + 1u);   /* AA cmd len <payload> sum */

    dy_drain(dev);

    if (!dy_send(dev, cmd, NULL, 0u))
    {
        return false;
    }

    start = dy_now(dev);

    while (!dy_elapsed(dev, start, dev->reply_ms))
    {
        byte = dy_rx(dev);

        if (byte < 0)
        {
            /* Nothing yet. Yield if we can; a 1 ms sleep is about one byte
             * time at 9600 baud, so this costs no throughput. */
            if (dev->delay_ms != NULL)
            {
                dev->delay_ms(1u);
            }
            continue;
        }

        if (n == (uint8_t)sizeof(buf))
        {
            /* Scratch full and no frame in it yet. Every complete window has
             * already been scanned, so anything still capable of becoming one
             * must start inside the last need-1 bytes. Keep those, drop the
             * rest, and carry on - the wait stays bounded by reply_ms and not
             * also by how much noise happened to arrive first. */
            uint8_t keep = (uint8_t)(need - 1u);
            uint8_t k;

            for (k = 0u; k < keep; k++)
            {
                buf[k] = buf[(n - keep) + k];
            }

            n = keep;
        }

        buf[n++] = (uint8_t)byte;

        if (n < need)
        {
            continue;
        }

        for (i = 0u; (uint8_t)(i + need) <= n; i++)
        {
            uint8_t sum = 0u;
            uint8_t k;

            if ((buf[i] != DY_FRAME_START) ||
                (buf[i + 1u] != cmd) ||
                (buf[i + 2u] != want_len))
            {
                continue;
            }

            for (k = i; k < (uint8_t)(i + need - 1u); k++)
            {
                sum = (uint8_t)(sum + buf[k]);
            }

            if (sum != buf[i + need - 1u])
            {
                continue;
            }

            if (out != NULL)
            {
                *out = (want_len == 2u)
                     ? (uint16_t)(((uint16_t)buf[i + 3u] << 8) | (uint16_t)buf[i + 4u])
                     : (uint16_t)buf[i + 3u];
            }

            return true;
        }
    }

    return false;
}

/*
 * The one path every query in this driver takes - the confirming 0x0D inside a
 * cue included, so a cue keeps the snapshot current for free.
 *
 * The timestamp is taken BEFORE the frame goes out, not after the reply lands.
 * A reply that took the full reply_ms should not read as 120 ms fresher than
 * the question that produced it.
 */
static bool dy_ask_now(dy_sound_t *dev, uint8_t cmd, uint8_t want_len,
                       uint8_t kind, uint16_t *out)
{
    uint16_t value = 0u;
    uint32_t at;
    bool     ok;

    if (kind == DY_REC_TRACK)
    {
        dy_clear_ask(dev, (uint8_t)DY_ASK_TRACK);
    }
    else if (kind == DY_REC_STATUS)
    {
        dy_clear_ask(dev, (uint8_t)DY_ASK_STATUS);
    }
    else
    {
        /* DY_REC_NONE answers no request. */
    }

    at = dy_now(dev);
    ok = dy_query(dev, cmd, want_len, &value);

    dy_record(dev, kind, ok, value, at);

    if (ok && (out != NULL))
    {
        *out = value;
    }

    return ok;
}

/* -- Setup ------------------------------------------------------------------ */

void dy_sound_cfg_init(dy_sound_cfg_t *cfg)
{
    uint8_t *p;
    uint32_t i;

    if (cfg == NULL)
    {
        return;
    }

    /* No <string.h> - this library pulls in nothing the RTOS heap rule would
     * object to, and a struct this size is not worth a memset dependency. */
    p = (uint8_t *)cfg;
    for (i = 0u; i < sizeof(*cfg); i++)
    {
        p[i] = 0u;
    }

    cfg->volume          = DY_VOLUME_DEFAULT;
    cfg->confirm_retries = DY_CONFIRM_RETRIES_DEF;
    cfg->boot_ms         = DY_BOOT_MS_DEFAULT;
    cfg->gap_ms          = DY_GAP_MS_DEFAULT;
    cfg->reply_ms        = DY_REPLY_MS_DEFAULT;
    cfg->settle_ms       = DY_SETTLE_MS_DEFAULT;
}

bool dy_sound_init(dy_sound_t *dev, const dy_sound_cfg_t *cfg)
{
    bool has_override;

    if ((dev == NULL) || (cfg == NULL))
    {
        return false;
    }

    /* Every wait in the driver is bounded by this. Without it there is no
     * gap, no reply window and no TX timeout, so refuse rather than run
     * something that only looks like it works. */
    if (cfg->now_ms == NULL)
    {
        return false;
    }

    has_override = (cfg->write != NULL) && (cfg->read_byte != NULL);

    /* Half an override is a mistake, not a configuration: writing through the
     * PLIB while reading the raw register (or the reverse) fights whatever
     * owns the other half. */
    if (!has_override && ((cfg->write != NULL) || (cfg->read_byte != NULL)))
    {
        return false;
    }

    if (!has_override && (cfg->sercom == NULL))
    {
        return false;
    }

    dev->sercom     = cfg->sercom;
    dev->write      = cfg->write;
    dev->read_byte  = cfg->read_byte;
    dev->io_context = cfg->io_context;
    dev->now_ms     = cfg->now_ms;
    dev->delay_ms   = cfg->delay_ms;
    dev->loop_rule  = cfg->loop_rule;
    dev->on_event   = cfg->on_event;
    dev->context    = cfg->context;

    /* Zero means "default" for the timing knobs - all four are durations where
     * zero has no useful meaning. `volume` is deliberately not in this list:
     * 0 is a legal volume, so a zeroed cfg is silent, not loud. */
    dev->boot_ms    = (cfg->boot_ms   != 0u) ? cfg->boot_ms   : (uint16_t)DY_BOOT_MS_DEFAULT;
    dev->gap_ms     = (cfg->gap_ms    != 0u) ? cfg->gap_ms    : (uint16_t)DY_GAP_MS_DEFAULT;
    dev->reply_ms   = (cfg->reply_ms  != 0u) ? cfg->reply_ms  : (uint16_t)DY_REPLY_MS_DEFAULT;
    dev->settle_ms  = (cfg->settle_ms != 0u) ? cfg->settle_ms : (uint16_t)DY_SETTLE_MS_DEFAULT;

    dev->volume          = (cfg->volume > 30u) ? 30u : cfg->volume;
    dev->confirm_retries = cfg->confirm_retries;

    /* The module powers up in single-stop. Recording that here means the first
     * ordinary cue does not send a redundant mode command. */
    dev->cycle_now = (uint8_t)DY_CYCLE_ONE_OFF;
    dev->ready     = false;
    dev->init_ms   = cfg->now_ms();

    /* Nothing has been asked yet, so the snapshot says exactly that rather than
     * a plausible-looking zero. Track 0 is "no track" and status 0 is
     * "stopped"; both would be answers we have not earned. */
    dev->status_seq = 0u;
    dev->confirmed  = -1;
    dev->last_track = -1;
    dev->status     = (uint8_t)DY_STATUS_UNKNOWN;
    dev->answered   = false;
    dev->asked_ms   = dev->init_ms;
    dev->queries    = 0u;
    dev->answers    = 0u;
    dev->ask_flags  = 0u;
    dev->bank      = NULL;
    dev->selected  = 0u;
    dev->rotation  = 0u;
    dev->queue     = NULL;
    dev->task      = NULL;
    dev->submit    = NULL;

    return true;
}

void dy_sound_begin(dy_sound_t *dev)
{
    uint32_t served;
    uint8_t  arg;

    if (dev == NULL)
    {
        return;
    }

    /* The card-mount window runs from power-on, and the application has
     * usually spent some of it initialising. Only wait out the remainder.
     *
     * Called before the scheduler starts with xTaskGetTickCount as the time
     * source, this reads zero elapsed and waits the full boot_ms, which is the
     * safe direction to be wrong in. */
    served = (uint32_t)(dy_now(dev) - dev->init_ms);

    if (served < dev->boot_ms)
    {
        dy_wait(dev, dev->boot_ms - served);
    }

    arg = dev->volume;
    (void)dy_send(dev, DY_CMD_SET_VOLUME, &arg, 1u);
    dy_wait(dev, dev->settle_ms);

    arg = dev->cycle_now;
    (void)dy_send(dev, DY_CMD_SET_CYCLE, &arg, 1u);
    dy_wait(dev, dev->settle_ms);

    dev->ready = true;
    dy_emit(dev, DY_EVENT_READY, 0u, (int)dev->volume);
}

/* -- Playback --------------------------------------------------------------- */

static bool dy_send_play(dy_sound_t *dev, uint16_t track)
{
    uint8_t payload[2];

    payload[0] = (uint8_t)(track >> 8);
    payload[1] = (uint8_t)(track & 0xFFu);

    return dy_send(dev, DY_CMD_SELECT_PLAY, payload, 2u);
}

/* Cycle mode is a module setting, not an argument to play, so it is only sent
 * when the answer actually changes - a cue after an ambient bed, or a bed after
 * a cue.
 *
 * `cycle` names the mode outright, or DY_CYCLE_KEEP to ask the loop rule. With
 * neither a stated mode nor a rule, nothing is sent and whatever the module was
 * last told stands. */
static void dy_apply_cycle(dy_sound_t *dev, uint16_t track, uint8_t cycle)
{
    uint8_t want;

    if (cycle != (uint8_t)DY_CYCLE_KEEP)
    {
        if (cycle > DY_CYCLE_RANDOM)
        {
            return;
        }
        want = cycle;
    }
    else if (dev->loop_rule != NULL)
    {
        want = dev->loop_rule(dev->context, track) ? (uint8_t)DY_CYCLE_LOOP_ONE
                                                   : (uint8_t)DY_CYCLE_ONE_OFF;
    }
    else
    {
        return;
    }

    if (want == dev->cycle_now)
    {
        return;
    }

    if (dy_send(dev, DY_CMD_SET_CYCLE, &want, 1u))
    {
        dev->cycle_now = want;
        dy_wait(dev, 20u);
    }
}

bool dy_sound_play_nowait(dy_sound_t *dev, uint16_t track)
{
    return dy_sound_play_ex(dev, track, (uint8_t)DY_CYCLE_KEEP, false);
}

bool dy_sound_play(dy_sound_t *dev, uint16_t track)
{
    return dy_sound_play_ex(dev, track, (uint8_t)DY_CYCLE_KEEP, true);
}

bool dy_sound_play_ex(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm)
{
    uint16_t reported;
    uint8_t  tries = 0u;
    bool     answered;

    if ((dev == NULL) || (track == 0u))
    {
        return false;
    }

    dy_apply_cycle(dev, track, cycle);

    if (!dy_send_play(dev, track))
    {
        return false;
    }

    /* Whatever the snapshot said is now about the previous clip. */
    dy_invalidate(dev);

    if (!confirm)
    {
        /* Fire and forget: one frame out, no gap, no reply window, back in
         * microseconds. dy_sound_status() will read track -1 and answered
         * false until something asks - which is the honest answer, because
         * nothing has. */
        return true;
    }

    /* Let the module seek and start before asking it anything. In a superloop
     * this wait is what pushes people into guessing from an acknowledgement
     * that does not exist. Here it is just a wait. */
    dy_wait(dev, dev->gap_ms);

    answered = dy_ask_now(dev, DY_CMD_Q_CURRENT_SONG, 2u, DY_REC_TRACK, &reported);

    /* The rule. A re-send needs a valid reply naming a DIFFERENT track.
     * Silence falls straight through and is never retried. */
    while (answered && (reported != track) && (tries < dev->confirm_retries))
    {
        tries++;

        if (!dy_send_play(dev, track))
        {
            return false;
        }

        /* Same reason as the first send: the answer that triggered this re-send
         * described the clip we have just replaced. */
        dy_invalidate(dev);

        dy_wait(dev, dev->gap_ms);
        answered = dy_ask_now(dev, DY_CMD_Q_CURRENT_SONG, 2u, DY_REC_TRACK, &reported);
    }

    /* The snapshot is already written - dy_ask_now did it, for the same reason
     * every other query goes through there. All that is left is to say what
     * happened. */
    if (!answered)
    {
        dy_emit(dev, DY_EVENT_NO_REPLY, track, -1);
        return true;
    }

    if (reported == track)
    {
        dy_emit(dev, DY_EVENT_CONFIRMED, track, (int)tries);
        return true;
    }

    dy_emit(dev, DY_EVENT_MISMATCH, track, (int)reported);
    return false;
}

/* Stop, pause, next and previous all change what the module is doing, and none
 * of them are answered - so the snapshot goes back to "unknown" rather than
 * keeping an answer that has just been made wrong. */
static bool dy_send_control(dy_sound_t *dev, uint8_t cmd)
{
    if ((dev == NULL) || !dy_send(dev, cmd, NULL, 0u))
    {
        return false;
    }

    dy_invalidate(dev);
    return true;
}

bool dy_sound_stop(dy_sound_t *dev)
{
    return dy_send_control(dev, DY_CMD_STOP);
}

bool dy_sound_pause(dy_sound_t *dev)
{
    return dy_send_control(dev, DY_CMD_PAUSE);
}

bool dy_sound_next(dy_sound_t *dev)
{
    return dy_send_control(dev, DY_CMD_NEXT);
}

bool dy_sound_previous(dy_sound_t *dev)
{
    return dy_send_control(dev, DY_CMD_PREVIOUS);
}

bool dy_sound_set_volume(dy_sound_t *dev, uint8_t volume)
{
    uint8_t arg;

    if (dev == NULL)
    {
        return false;
    }

    arg = (volume > 30u) ? 30u : volume;   /* clamp at the boundary, not later */

    if (!dy_send(dev, DY_CMD_SET_VOLUME, &arg, 1u))
    {
        return false;
    }

    dev->volume = arg;
    dy_wait(dev, dev->settle_ms);
    return true;
}

bool dy_sound_set_cycle(dy_sound_t *dev, uint8_t mode)
{
    if ((dev == NULL) || (mode > DY_CYCLE_RANDOM))
    {
        return false;
    }

    if (!dy_send(dev, DY_CMD_SET_CYCLE, &mode, 1u))
    {
        return false;
    }

    dev->cycle_now = mode;
    dy_wait(dev, dev->settle_ms);
    return true;
}

/* -- Queries ---------------------------------------------------------------- */

bool dy_sound_query_track(dy_sound_t *dev, uint16_t *out)
{
    return (dev != NULL) && dy_ask_now(dev, DY_CMD_Q_CURRENT_SONG, 2u, DY_REC_TRACK, out);
}

bool dy_sound_query_status(dy_sound_t *dev, uint8_t *out)
{
    uint16_t raw;

    if ((dev == NULL) || !dy_ask_now(dev, DY_CMD_Q_STATUS, 1u, DY_REC_STATUS, &raw))
    {
        return false;
    }

    if (out != NULL)
    {
        *out = (uint8_t)raw;
    }

    return true;
}

bool dy_sound_query_song_count(dy_sound_t *dev, uint16_t *out)
{
    return (dev != NULL) && dy_ask_now(dev, DY_CMD_Q_SONG_COUNT, 2u, DY_REC_NONE, out);
}

int dy_sound_confirmed(const dy_sound_t *dev)
{
    return (dev != NULL) ? (int)dev->confirmed : -1;
}

/* -- The snapshot, and asking for a fresh one ------------------------------- */

bool dy_sound_status(const dy_sound_t *dev, dy_status_t *out)
{
    uint32_t tries;

    if (out == NULL)
    {
        return false;
    }

    if (dev == NULL)
    {
        out->track      = -1;
        out->last_track = -1;
        out->status     = (uint8_t)DY_STATUS_UNKNOWN;
        out->playing    = false;
        out->answered   = false;
        out->asked_ms   = 0u;
        out->queries    = 0u;
        out->answers    = 0u;
        return false;
    }

    for (tries = 0u; tries < DY_STATUS_READ_TRIES; tries++)
    {
        uint32_t before = dev->status_seq;

        /* Odd means a writer is inside dy_record right now. Nothing read here
         * would be trustworthy, so do not even copy it. */
        if ((before & 1u) != 0u)
        {
            continue;
        }

        DY_BARRIER();

        out->track      = dev->confirmed;
        out->last_track = dev->last_track;
        out->status     = dev->status;
        out->answered   = dev->answered;
        out->asked_ms   = dev->asked_ms;
        out->queries    = dev->queries;
        out->answers    = dev->answers;
        out->playing    = (out->status == (uint8_t)DY_STATUS_PLAYING);

        DY_BARRIER();

        if (dev->status_seq == before)
        {
            return true;
        }
    }

    /* Torn. *out still holds the last attempt's fields, each of which is
     * individually sound - they are just not guaranteed to be from one query.
     * Handing back something usable and saying so beats handing back nothing:
     * the caller that only wanted `playing` is fine either way, and the one
     * comparing counters now knows not to. */
    return false;
}

void dy_sound_ask(dy_sound_t *dev, uint8_t what)
{
    uint32_t saved;

    what &= (uint8_t)DY_ASK_BOTH;

    if ((dev == NULL) || (what == 0u))
    {
        return;
    }

    /* OR, not assign. Two edges before the task gets a turn are one query, and
     * a status request must not erase a track request that arrived first. */
    saved = dy_irq_save();
    dev->ask_flags = (uint8_t)(dev->ask_flags | what);
    dy_irq_restore(saved);
}

uint8_t dy_sound_ask_pending(const dy_sound_t *dev)
{
    return (dev != NULL) ? dev->ask_flags : 0u;
}

uint8_t dy_sound_refresh(dy_sound_t *dev, uint8_t what)
{
    uint8_t served = 0u;

    what &= (uint8_t)DY_ASK_BOTH;

    if ((dev == NULL) || (what == 0u))
    {
        return 0u;
    }

    /* Track first. It is the question a BUSY edge is really asking, and if the
     * module is going to answer only one of the two, this is the useful one. */
    if (((what & (uint8_t)DY_ASK_TRACK) != 0u) && dy_sound_query_track(dev, NULL))
    {
        served |= (uint8_t)DY_ASK_TRACK;
    }

    if (((what & (uint8_t)DY_ASK_STATUS) != 0u) && dy_sound_query_status(dev, NULL))
    {
        served |= (uint8_t)DY_ASK_STATUS;
    }

    return served;
}

uint8_t dy_sound_service(dy_sound_t *dev)
{
    uint8_t what;

    if (dev == NULL)
    {
        return 0u;
    }

    /* Taken and cleared before the queries run, not after. A request raised
     * while we are on the wire is a NEW request about a newer state, and
     * clearing afterwards would swallow it. */
    what = dy_take_ask(dev);

    if (what == 0u)
    {
        return 0u;
    }

    return dy_sound_refresh(dev, what);
}

/* -- Escape hatch ----------------------------------------------------------- */

bool dy_sound_send_raw(dy_sound_t *dev, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    return (dev != NULL) && dy_send(dev, cmd, data, len);
}

bool dy_sound_query_raw(dy_sound_t *dev, uint8_t cmd, uint8_t want_len, uint16_t *out)
{
    /* Counted and timestamped like any other query, but it names no snapshot
     * field: this driver has no idea what an arbitrary command's payload
     * means. */
    return (dev != NULL) && dy_ask_now(dev, cmd, want_len, DY_REC_NONE, out);
}
