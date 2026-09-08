/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound_freertos.c - optional FreeRTOS task layer
 * Compiles to nothing unless DY_SOUND_ENABLE_FREERTOS is defined.
 * ============================================================================= */

#ifdef DY_SOUND_ENABLE_FREERTOS

#include "dy_sound/dy_sound_freertos.h"

#include "queue.h"

/* Every producer call funnels into one of these and is executed by the sound
 * task, so the port only ever has one writer. Routing volume and cycle through
 * the queue as well is the point: called directly from another task they would
 * interleave with a frame already going out. */
typedef enum
{
    DY_OP_PLAY = 0,
    DY_OP_PLAY_NOWAIT,
    DY_OP_STOP,
    DY_OP_PAUSE,
    DY_OP_NEXT,
    DY_OP_PREVIOUS,
    DY_OP_SET_VOLUME,
    DY_OP_SET_CYCLE
} dy_op_t;

typedef struct
{
    uint16_t track;   /**< DY_OP_PLAY / DY_OP_PLAY_NOWAIT                     */
    uint8_t  op;
    uint8_t  arg;     /**< volume, cycle mode, or the play's own cycle mode.
                       *   Carrying it here is what lets the bank layer decide
                       *   cycle mode in the producer's task while the sound
                       *   task remains the only thing that writes to the port. */
} dy_cue_t;

static bool dy_rtos_submit(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm);

void dy_sound_rtos_delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    /* pdMS_TO_TICKS truncates, so a sub-tick request would otherwise not wait
     * at all. Round up to one tick: every caller here wants "at least this
     * long", never "at most". */
    if ((ticks == 0u) && (ms != 0u))
    {
        ticks = 1u;
    }

    vTaskDelay(ticks);
}

static bool dy_post(dy_sound_t *dev, const dy_cue_t *cue)
{
    if ((dev == NULL) || (dev->queue == NULL))
    {
        return false;
    }

    /* Zero ticks: drop the cue rather than stall whatever task is running the
     * room. A prop that pauses is worse than a cue that is missed. */
    if (xQueueSend((QueueHandle_t)dev->queue, cue, 0) != pdPASS)
    {
        if (dev->on_event != NULL)
        {
            dev->on_event(dev->context, DY_EVENT_DROPPED, cue->track, -1);
        }
        return false;
    }

    return true;
}

static void dy_run(void *arg)
{
    dy_sound_t *dev = (dy_sound_t *)arg;
    dy_cue_t    cue;

    /* Waits out whatever is left of the module's card-mount time, then sets
     * volume and cycle mode and emits DY_EVENT_READY. */
    dy_sound_begin(dev);

    for (;;)
    {
        /* Sleeps until there is work. This is the only place the task is
         * allowed to be idle, and it costs nothing while it is. */
        if (xQueueReceive((QueueHandle_t)dev->queue, &cue, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        switch ((dy_op_t)cue.op)
        {
            /* play_ex, not the submit hook - the hook is how cues get HERE.
             * Routing through it again would post straight back to this queue. */
            case DY_OP_PLAY:
                (void)dy_sound_play_ex(dev, cue.track, cue.arg, true);
                break;

            case DY_OP_PLAY_NOWAIT:
                (void)dy_sound_play_ex(dev, cue.track, cue.arg, false);
                break;

            case DY_OP_STOP:
                (void)dy_sound_stop(dev);
                break;

            case DY_OP_PAUSE:
                (void)dy_sound_pause(dev);
                break;

            case DY_OP_NEXT:
                (void)dy_sound_next(dev);
                break;

            case DY_OP_PREVIOUS:
                (void)dy_sound_previous(dev);
                break;

            case DY_OP_SET_VOLUME:
                (void)dy_sound_set_volume(dev, cue.arg);
                break;

            case DY_OP_SET_CYCLE:
            default:
                (void)dy_sound_set_cycle(dev, cue.arg);
                break;
        }
    }
}

void dy_sound_rtos_cfg_init(dy_sound_rtos_cfg_t *cfg)
{
    if (cfg == NULL)
    {
        return;
    }

    cfg->name        = "dysound";
    cfg->stack_words = (uint16_t)DY_TASK_STACK_DEFAULT;
    cfg->queue_len   = (uint16_t)DY_QUEUE_LEN_DEFAULT;
    cfg->priority    = (UBaseType_t)DY_TASK_PRIORITY_DEFAULT;
}

bool dy_sound_rtos_start(dy_sound_t *dev, const dy_sound_rtos_cfg_t *cfg)
{
    dy_sound_rtos_cfg_t defaults;
    QueueHandle_t       queue;
    TaskHandle_t        task = NULL;
    const char         *name;
    uint16_t            stack_words;
    uint16_t            queue_len;

    if (dev == NULL)
    {
        return false;
    }

    /* dy_sound_init() clears these, so a non-NULL queue means this device is
     * already owned by a task. Starting a second one would put two writers on
     * the port, which is the exact thing the task exists to prevent. */
    if (dev->queue != NULL)
    {
        return false;
    }

    if (cfg == NULL)
    {
        dy_sound_rtos_cfg_init(&defaults);
        cfg = &defaults;
    }

    name        = (cfg->name != NULL)     ? cfg->name        : "dysound";
    stack_words = (cfg->stack_words != 0u) ? cfg->stack_words : (uint16_t)DY_TASK_STACK_DEFAULT;
    queue_len   = (cfg->queue_len != 0u)   ? cfg->queue_len   : (uint16_t)DY_QUEUE_LEN_DEFAULT;

    queue = xQueueCreate((UBaseType_t)queue_len, sizeof(dy_cue_t));

    if (queue == NULL)
    {
        return false;
    }

    /* The driver's waits become vTaskDelay from here on, so the boot wait and
     * every inter-frame gap yield the CPU instead of spinning on now_ms. */
    dev->delay_ms = dy_sound_rtos_delay_ms;
    dev->queue    = (void *)queue;

    /* From here the bank layer posts instead of transmitting, so
     * dy_sound_bank_play() is safe from any task. */
    dev->submit   = dy_rtos_submit;

    /* Compared against pdPASS explicitly. Rolling the result up with &= would
     * be wrong: the failure code is errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY (-1),
     * and -1 & 1 is 1, so a bitwise accumulator reports success on failure. */
    if (xTaskCreate(dy_run, name, stack_words, dev, cfg->priority, &task) != pdPASS)
    {
        vQueueDelete(queue);
        dev->queue    = NULL;
        dev->delay_ms = NULL;
        dev->submit   = NULL;
        return false;
    }

    dev->task = (void *)task;
    return true;
}

/* -- Producer API ----------------------------------------------------------- */

static bool dy_post_simple(dy_sound_t *dev, dy_op_t op, uint16_t track, uint8_t arg)
{
    dy_cue_t cue;

    cue.track = track;
    cue.op    = (uint8_t)op;
    cue.arg   = arg;

    return dy_post(dev, &cue);
}

bool dy_sound_rtos_play(dy_sound_t *dev, uint16_t track)
{
    return dy_sound_rtos_play_ex(dev, track, (uint8_t)DY_CYCLE_KEEP, true);
}

bool dy_sound_rtos_play_nowait(dy_sound_t *dev, uint16_t track)
{
    return dy_sound_rtos_play_ex(dev, track, (uint8_t)DY_CYCLE_KEEP, false);
}

bool dy_sound_rtos_play_ex(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm)
{
    if (track == 0u)
    {
        return false;
    }

    return dy_post_simple(dev, confirm ? DY_OP_PLAY : DY_OP_PLAY_NOWAIT, track, cycle);
}

/* The submit hook the bank layer calls. Same signature as dy_sound_play_ex, so
 * the bank can use one code path whether or not a sound task exists. */
static bool dy_rtos_submit(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm)
{
    return dy_sound_rtos_play_ex(dev, track, cycle, confirm);
}

bool dy_sound_rtos_stop(dy_sound_t *dev)
{
    return dy_post_simple(dev, DY_OP_STOP, 0u, 0u);
}

bool dy_sound_rtos_pause(dy_sound_t *dev)
{
    return dy_post_simple(dev, DY_OP_PAUSE, 0u, 0u);
}

bool dy_sound_rtos_next(dy_sound_t *dev)
{
    return dy_post_simple(dev, DY_OP_NEXT, 0u, 0u);
}

bool dy_sound_rtos_previous(dy_sound_t *dev)
{
    return dy_post_simple(dev, DY_OP_PREVIOUS, 0u, 0u);
}

bool dy_sound_rtos_set_volume(dy_sound_t *dev, uint8_t volume)
{
    return dy_post_simple(dev, DY_OP_SET_VOLUME, 0u, (volume > 30u) ? 30u : volume);
}

bool dy_sound_rtos_set_cycle(dy_sound_t *dev, uint8_t mode)
{
    if (mode > DY_CYCLE_RANDOM)
    {
        return false;
    }

    return dy_post_simple(dev, DY_OP_SET_CYCLE, 0u, mode);
}

bool dy_sound_rtos_flush(dy_sound_t *dev)
{
    if ((dev == NULL) || (dev->queue == NULL))
    {
        return false;
    }

    /* Only the backlog. A cue already handed to the module keeps playing -
     * stopping it is dy_sound_rtos_stop()'s job, and conflating the two would
     * make a queue reset audible. */
    return xQueueReset((QueueHandle_t)dev->queue) == pdPASS;
}

uint32_t dy_sound_rtos_pending(const dy_sound_t *dev)
{
    if ((dev == NULL) || (dev->queue == NULL))
    {
        return 0u;
    }

    return (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)dev->queue);
}

TaskHandle_t dy_sound_rtos_task(const dy_sound_t *dev)
{
    return (dev != NULL) ? (TaskHandle_t)dev->task : NULL;
}

#else  /* DY_SOUND_ENABLE_FREERTOS */

/* ISO C forbids an empty translation unit. */
typedef int dy_sound_freertos_not_enabled_t;

#endif /* DY_SOUND_ENABLE_FREERTOS */
