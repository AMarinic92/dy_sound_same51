/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound_bank.h - table-driven sound definitions
 *
 * WHAT THIS REPLACES
 * ------------------
 * Without it, a room's sounds end up spread across three places that have to be
 * kept in agreement by hand:
 *
 *   1. a switch mapping cue codes to track indices
 *          case 250: return 3;   // platform change (alternates with 251)
 *          case 251: return 4;   // platform change
 *          case 252: return 5;   // ambient         (alternates with 253)
 *
 *   2. a separate predicate deciding which tracks repeat
 *          static bool mastLoops(uint8_t t) { return t == 6; }
 *
 *   3. a latch pair per module, plus the edge test, in the application
 *          if (SoundVariable != LastSoundVariable) {
 *              Mast.play(SoundVariable);
 *              LastSoundVariable = SoundVariable;
 *          }
 *
 * Adding one sound means editing all three, and the fact that track 6 is an
 * ambient bed is stated in (2) with no visible connection to (1). Worse, (3) is
 * where the original bug lived: advance the latch in the wrong place and the
 * edge is eaten, so the cue never plays at all.
 *
 * A bank states each sound once:
 *
 *     enum { SND_WIND = 1, SND_PLATFORM, SND_AMBIENT, SND_CANNON };
 *
 *     static const dy_track_t chest_tracks[] = {
 *         DY_TRACK    (SND_WIND,      1, "wind"              ),
 *         DY_TRACK    (SND_PLATFORM,  3, "platform change"   ),
 *         DY_TRACK    (SND_PLATFORM,  4, "platform change b" ),
 *         DY_TRACK_BED(SND_AMBIENT,   5, "ambient bed"       ),
 *         DY_TRACK    (SND_CANNON,    8, "cannon fire"       ),
 *     };
 *     static const dy_bank_t chest_bank = DY_BANK(chest_tracks);
 *
 * Index, repeat behaviour and label sit on one line. Two lines sharing an id
 * are variants and alternate round-robin, which is what the 250/251 pair was
 * doing by hand. And the latch moves into the driver:
 *
 *     dy_sound_bank_select(&chest, SND_AMBIENT);   // plays on a change only
 *     dy_sound_bank_select(&chest, SND_AMBIENT);   // does nothing
 *
 * WORKS WITH OR WITHOUT THE SOUND TASK
 * ------------------------------------
 * Every call here resolves the id against the table - pure lookup, no I/O - and
 * then hands the result to whichever path the device is set up for. Started
 * with dy_sound_rtos_start(), that is a queue post and the call does not block.
 * On bare metal it is the cue itself. The calling code reads the same either
 * way, so you can use the bank from any task.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * Decide anything. The bank moves a room's vocabulary into one table; it has no
 * opinion about which sound belongs to which puzzle state. That stays in the
 * application, where it can be read.
 * ============================================================================= */

#ifndef DY_SOUND_BANK_H
#define DY_SOUND_BANK_H

#include "dy_sound/dy_sound.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set to 0 in your toolchain's preprocessor macros to drop every label string
 * from the image. The DY_TRACK macros still take the argument - it is discarded
 * at compile time - so nothing else changes and the table stays readable.
 */
#ifndef DY_SOUND_LABELS
#define DY_SOUND_LABELS 1
#endif

/** Reserved id meaning "no sound". Never matches an entry. */
#define DY_SOUND_NONE   0u

typedef struct
{
    uint16_t    id;      /**< what the application asks for. 0 is reserved.  */
    uint16_t    track;   /**< index on the card, 1-based. See the warning in
                          *   dy_sound.h about copy order.                    */
    uint8_t     cycle;   /**< DY_CYCLE_ONE_OFF, DY_CYCLE_LOOP_ONE, ...        */
#if DY_SOUND_LABELS
    const char *label;   /**< for logging. Never compared against.            */
#endif
} dy_track_t;

typedef struct
{
    const dy_track_t *tracks;
    uint16_t          count;
} dy_bank_t;

/* -- Declaring a bank -------------------------------------------------------
 * DY_TRACK      plays once and stops - almost everything
 * DY_TRACK_BED  repeats until something else is played - ambient beds
 * DY_TRACK_MODE spells the cycle mode out, for the two rarer modes
 *
 * Repeat an id to declare variants of the same cue. They are handed out
 * round-robin, so an ambient that alternates between two beds, or a hint line
 * that should not repeat verbatim, is two lines and no application state.
 * -------------------------------------------------------------------------- */

#if DY_SOUND_LABELS
#define DY_TRACK_MODE(id_, track_, cycle_, label_)  { (id_), (track_), (cycle_), (label_) }
#else
#define DY_TRACK_MODE(id_, track_, cycle_, label_)  { (id_), (track_), (cycle_) }
#endif

#define DY_TRACK(id_, track_, label_)      DY_TRACK_MODE((id_), (track_), DY_CYCLE_ONE_OFF,  (label_))
#define DY_TRACK_BED(id_, track_, label_)  DY_TRACK_MODE((id_), (track_), DY_CYCLE_LOOP_ONE, (label_))

/** Wrap a dy_track_t array into a bank. The array must be in scope. */
#define DY_BANK(array_)  { (array_), (uint16_t)(sizeof(array_) / sizeof((array_)[0])) }

/* -- Attaching -------------------------------------------------------------- */

/**
 * Point a device at a bank. Call after dy_sound_init(). The bank is only read,
 * so keep it `const` and let it live in flash.
 *
 * This clears any loop rule on the device: cycle mode now comes from the
 * table's `cycle` column, and two mechanisms deciding it would fight. Returns
 * false on a NULL or empty bank.
 */
bool dy_sound_bank_attach(dy_sound_t *dev, const dy_bank_t *bank);

/* -- Playing ----------------------------------------------------------------
 * All of these are safe from any task once dy_sound_rtos_start() has run. On
 * bare metal they cost a full cue - see dy_sound_play().
 * -------------------------------------------------------------------------- */

/**
 * Play the sound registered under `id`, unconditionally, whether or not it is
 * already the selected one. Advances the variant rotation if the id has more
 * than one entry.
 *
 * Returns false if the id is not in the bank - which is the useful failure,
 * because a cue code nobody registered is a typo, not a silence.
 */
bool dy_sound_bank_play(dy_sound_t *dev, uint16_t id);

/**
 * Say what the room's sound should now be. Plays only when `id` differs from
 * the last id selected, so this is safe to call every pass of a loop - which is
 * the whole point, and what removes the LastSoundVariable latch from the
 * application.
 *
 * Selecting DY_SOUND_NONE records that nothing is cued and sends nothing. It
 * does not stop what is playing: a cue that has already started should finish,
 * and killing it here would make every "sound is over" transition audible. Call
 * dy_sound_stop() when you actually mean stop.
 *
 * Returns true when a cue was sent.
 */
bool dy_sound_bank_select(dy_sound_t *dev, uint16_t id);

/** Play the selected id again, from the top. Use for a hint the player asked to
 *  repeat. Returns false if nothing is selected. */
bool dy_sound_bank_replay(dy_sound_t *dev);

/**
 * Forget what is selected without sending anything, so the next select() of the
 * same id counts as a change and plays. Use on a room reset, where the previous
 * game's last cue must not suppress the new game's first one.
 */
void dy_sound_bank_reset(dy_sound_t *dev);

/* -- Introspection ---------------------------------------------------------- */

/** The id currently selected, or DY_SOUND_NONE. */
uint16_t dy_sound_bank_selected(const dy_sound_t *dev);

/**
 * Find an id's first entry, or NULL. For logging a cue by name, and for
 * asserting at startup that a table someone edited still has what the code
 * expects.
 */
const dy_track_t *dy_sound_bank_find(const dy_sound_t *dev, uint16_t id);

/** How many variants an id has. 0 means it is not in the bank. */
uint16_t dy_sound_bank_variants(const dy_sound_t *dev, uint16_t id);

/**
 * An entry's label, or "" when labels are compiled out or the id is unknown.
 * Never returns NULL, so it is safe to hand straight to printf.
 */
const char *dy_sound_bank_label(const dy_sound_t *dev, uint16_t id);

/**
 * Ask the module how many files are on the card and check that every track in
 * the bank exists. Costs one query - about 10 ms when the module answers.
 *
 * Returns true if the card has at least as many files as the bank's highest
 * track index. Returns false if it has fewer, and writes the offending index to
 * *first_missing when that pointer is given.
 *
 * A module that does not answer cannot disprove anything, so silence returns
 * true and leaves *first_missing alone. Call it once at startup: it catches the
 * card copied in the wrong order, which otherwise shows up as every cue playing
 * the wrong clip.
 *
 * Drives the UART, so on a device with a sound task running, call it only from
 * that task - from the on_event handler for DY_EVENT_READY, for instance.
 */
bool dy_sound_bank_verify(dy_sound_t *dev, uint16_t *first_missing);

#ifdef __cplusplus
}
#endif

#endif /* DY_SOUND_BANK_H */
