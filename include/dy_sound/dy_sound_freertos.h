/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound_freertos.h - optional FreeRTOS task layer for the dy_sound driver
 *
 * One task per module. The task owns its SERCOM outright, so no locks are
 * needed on the hardware. Producers post a track and never block.
 *
 * WHY A TASK AND NOT A LOCK
 * -------------------------
 * A cue costs gap_ms plus the module's reply - about 210 ms when it answers,
 * 320 ms when it does not. Nothing that runs a prop can afford to sit in that
 * wait, and a mutex would only move the wait to whoever wanted the port next.
 *
 * With a task, the cost lands entirely inside the sound task. Every other task
 * calls dy_sound_rtos_play() and returns immediately. Because the task is the
 * only thing that ever touches the port, no two frames can interleave on the
 * wire, which is what the lock would have been for.
 *
 * ENABLING
 * --------
 * src/dy_sound_freertos.c compiles to nothing unless DY_SOUND_ENABLE_FREERTOS
 * is defined, so it is safe to leave in a glob-based file set on a bare-metal
 * project. Define it in your toolchain's preprocessor macros to switch on.
 *
 * REQUIREMENTS
 * ------------
 *   configSUPPORT_DYNAMIC_ALLOCATION  1
 *   INCLUDE_vTaskDelay                1
 *
 * No mutexes, no task notifications, no software timers - so nothing here
 * conflicts with a notification index another driver is already using, and
 * configUSE_RECURSIVE_MUTEXES may stay 0.
 *
 * A 1 kHz tick is assumed only in that dy_sound_rtos_delay_ms() and the device's
 * now_ms source must agree on milliseconds. xTaskGetTickCount() is the right
 * now_ms when configTICK_RATE_HZ is 1000.
 *
 * USAGE
 * -----
 *     static dy_sound_t mast;
 *
 *     static bool mast_loops(uintptr_t ctx, uint16_t track)
 *     {
 *         (void)ctx;
 *         return track == 6u;            // the ambient bed
 *     }
 *
 *     int main(void)
 *     {
 *         dy_sound_cfg_t cfg;
 *         dy_sound_rtos_cfg_t rtos;
 *
 *         SYS_Initialize(NULL);
 *
 *         dy_sound_cfg_init(&cfg);
 *         cfg.sercom    = SERCOM5_REGS;
 *         cfg.now_ms    = xTaskGetTickCount;
 *         cfg.loop_rule = mast_loops;
 *         dy_sound_init(&mast, &cfg);
 *
 *         dy_sound_rtos_cfg_init(&rtos);
 *         rtos.name = "sound";
 *         dy_sound_rtos_start(&mast, &rtos);   // before the scheduler
 *
 *         vTaskStartScheduler();
 *     }
 *
 *     dy_sound_rtos_play(&mast, 14);           // from anywhere; never blocks
 * ============================================================================= */

#ifndef DY_SOUND_FREERTOS_H
#define DY_SOUND_FREERTOS_H

#include "dy_sound/dy_sound.h"

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Cues that can wait. Past this the newest is dropped rather than blocking a
 *  prop, and DY_EVENT_DROPPED fires. */
#define DY_QUEUE_LEN_DEFAULT       8u

/** Task stack, in words. The driver itself is shallow - a 24-byte receive
 *  scratch and an 8-byte frame - but your on_event callback runs on this
 *  stack. Raise it if that callback calls printf. */
#define DY_TASK_STACK_DEFAULT    256u

/** Below the puzzle logic. A cue that lands 20 ms late is inaudible; a puzzle
 *  that runs 20 ms late is not. */
#define DY_TASK_PRIORITY_DEFAULT   1u

typedef struct
{
    const char *name;          /**< task name, keep it short. NULL -> "dysound" */
    uint16_t    stack_words;   /**< 0 -> DY_TASK_STACK_DEFAULT                  */
    uint16_t    queue_len;     /**< 0 -> DY_QUEUE_LEN_DEFAULT                   */
    UBaseType_t priority;      /**< 0 is legal but shares the idle priority     */
} dy_sound_rtos_cfg_t;

/** Fill a cfg with every default. Call this first, then override. */
void dy_sound_rtos_cfg_init(dy_sound_rtos_cfg_t *cfg);

/**
 * Create the queue and the task for a device that dy_sound_init() has already
 * bound. Call before vTaskStartScheduler(). Returns false if either allocation
 * failed, or if the device was already started.
 *
 * The task waits out the module's boot time, sets volume and cycle mode
 * (emitting DY_EVENT_READY), then sleeps on the queue until there is work.
 *
 * This installs a delay hook on the device so the driver's waits yield the CPU
 * instead of spinning. From that point the device belongs to the sound task:
 * drive it through the dy_sound_rtos_* calls below, not the direct API, or two
 * tasks will interleave bytes mid-frame.
 */
bool dy_sound_rtos_start(dy_sound_t *dev, const dy_sound_rtos_cfg_t *cfg);

/* -- Producer API -----------------------------------------------------------
 * Safe from any task. Each posts one cue and returns immediately - never
 * blocking, never waiting on the module. All of them return false if the queue
 * is full, having emitted DY_EVENT_DROPPED, or if the device was never started.
 *
 * Not safe from an ISR: these use the task-context queue calls.
 * -------------------------------------------------------------------------- */

/** Post a track. Track 0 means "nothing to play" and is rejected. */
bool dy_sound_rtos_play(dy_sound_t *dev, uint16_t track);

/** Post a track, skipping the confirming query. Cheaper, and the right call
 *  when the module's TX line is not connected. */
bool dy_sound_rtos_play_nowait(dy_sound_t *dev, uint16_t track);

/** Post a track with its cycle mode stated outright - the queued form of
 *  dy_sound_play_ex(). Pass DY_CYCLE_KEEP to leave it to the loop rule. */
bool dy_sound_rtos_play_ex(dy_sound_t *dev, uint16_t track, uint8_t cycle, bool confirm);

bool dy_sound_rtos_stop(dy_sound_t *dev);
bool dy_sound_rtos_pause(dy_sound_t *dev);
bool dy_sound_rtos_next(dy_sound_t *dev);
bool dy_sound_rtos_previous(dy_sound_t *dev);
bool dy_sound_rtos_set_volume(dy_sound_t *dev, uint8_t volume);
bool dy_sound_rtos_set_cycle(dy_sound_t *dev, uint8_t mode);

/**
 * Drop every cue still waiting, without disturbing whatever is playing. Use
 * when a room resets and the backlog is now wrong.
 */
bool dy_sound_rtos_flush(dy_sound_t *dev);

/* -- Introspection ---------------------------------------------------------- */

/** Cues waiting to be sent. 0 does not mean the module is silent - it means the
 *  task has nothing left to hand it. */
uint32_t dy_sound_rtos_pending(const dy_sound_t *dev);

/** The sound task, or NULL if it was never started. */
TaskHandle_t dy_sound_rtos_task(const dy_sound_t *dev);

/* -- Running without the task ----------------------------------------------- */

/**
 * The delay hook the task layer installs, exported so you can use the direct
 * dy_sound_* API from a task of your own without busy-waiting:
 *
 *     cfg.now_ms   = xTaskGetTickCount;
 *     cfg.delay_ms = dy_sound_rtos_delay_ms;
 *     dy_sound_init(&mast, &cfg);       // and never call rtos_start
 *
 * Then call dy_sound_begin() and dy_sound_play() from that one task. You give
 * up the non-blocking producer API and take the ~210 ms per cue yourself.
 *
 * Must not be called before the scheduler is running.
 */
void dy_sound_rtos_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* DY_SOUND_FREERTOS_H */
