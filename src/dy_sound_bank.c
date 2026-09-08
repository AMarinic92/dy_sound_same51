/* SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* =============================================================================
 * dy_sound_bank.c - table-driven sound definitions
 * See include/dy_sound/dy_sound_bank.h for what this replaces and why.
 * ============================================================================= */

#include "dy_sound/dy_sound_bank.h"

/* Lookups are a linear scan. A room's vocabulary is tens of entries, so this is
 * a handful of 16-bit compares against a table in flash - immaterial next to the
 * 200 ms the cue itself costs. Ordering the table for a binary search would buy
 * nothing and would make the id column something you had to maintain. */

static const dy_bank_t *bank_of(const dy_sound_t *dev)
{
    if ((dev == NULL) || (dev->bank == NULL))
    {
        return NULL;
    }

    return (const dy_bank_t *)dev->bank;
}

/*
 * Find the nth entry carrying `id`, wrapping. Returns NULL if the id is absent.
 *
 * Variants fall out of this: entries sharing an id are handed out in table
 * order, so declaring two lines with the same id is the whole feature. `which`
 * is taken modulo the count, so the caller's rotation cursor can just keep
 * counting up and never has to know how many variants exist.
 */
static const dy_track_t *find_nth(const dy_bank_t *bank, uint16_t id, uint16_t which,
                                  uint16_t *variants)
{
    const dy_track_t *first = NULL;
    uint16_t          count = 0u;
    uint16_t          i;

    for (i = 0u; i < bank->count; i++)
    {
        if (bank->tracks[i].id == id)
        {
            count++;
        }
    }

    if (variants != NULL)
    {
        *variants = count;
    }

    if (count == 0u)
    {
        return NULL;
    }

    which = (uint16_t)(which % count);
    count = 0u;

    for (i = 0u; i < bank->count; i++)
    {
        if (bank->tracks[i].id != id)
        {
            continue;
        }

        if (count == which)
        {
            return &bank->tracks[i];
        }

        count++;

        if (first == NULL)
        {
            first = &bank->tracks[i];
        }
    }

    return first;   /* unreachable while count is stable, but not assumed */
}

/*
 * Hand a resolved entry to whichever path this device is set up for.
 *
 * The submit hook is installed by dy_sound_rtos_start(), so on a device with a
 * sound task this posts to its queue and returns at once; on bare metal it is
 * the cue itself. Nothing above this function has to know which, which is what
 * lets the bank API be called from any task.
 */
static bool dispatch(dy_sound_t *dev, const dy_track_t *entry)
{
    if (dev->submit != NULL)
    {
        return dev->submit(dev, entry->track, entry->cycle, true);
    }

    return dy_sound_play_ex(dev, entry->track, entry->cycle, true);
}

bool dy_sound_bank_attach(dy_sound_t *dev, const dy_bank_t *bank)
{
    if ((dev == NULL) || (bank == NULL) || (bank->tracks == NULL) || (bank->count == 0u))
    {
        return false;
    }

    dev->bank     = (const void *)bank;
    dev->selected = (uint16_t)DY_SOUND_NONE;
    dev->rotation = 0u;

    /* Cycle mode now comes from the table. Leaving a loop rule installed would
     * put two mechanisms in charge of the same setting, and which one won would
     * depend on the order they happened to run in. */
    dev->loop_rule = NULL;

    return true;
}

bool dy_sound_bank_play(dy_sound_t *dev, uint16_t id)
{
    const dy_bank_t  *bank = bank_of(dev);
    const dy_track_t *entry;
    uint16_t          variants = 0u;

    if ((bank == NULL) || (id == (uint16_t)DY_SOUND_NONE))
    {
        return false;
    }

    entry = find_nth(bank, id, dev->rotation, &variants);

    if (entry == NULL)
    {
        return false;
    }

    /* Advance only when there is something to rotate through. Bumping the
     * cursor on every single-variant cue would still work - it is taken modulo
     * the count - but it would make the rotation of a two-variant id depend on
     * how many unrelated cues ran between its turns. */
    if (variants > 1u)
    {
        dev->rotation++;
    }

    return dispatch(dev, entry);
}

bool dy_sound_bank_select(dy_sound_t *dev, uint16_t id)
{
    if (bank_of(dev) == NULL)
    {
        return false;
    }

    if (id == dev->selected)
    {
        return false;
    }

    dev->selected = id;

    /* Recording "nothing is cued" is the point of allowing DY_SOUND_NONE: it
     * re-arms the edge so the same id can be selected again later and play. */
    if (id == (uint16_t)DY_SOUND_NONE)
    {
        return false;
    }

    return dy_sound_bank_play(dev, id);
}

bool dy_sound_bank_replay(dy_sound_t *dev)
{
    if (bank_of(dev) == NULL)
    {
        return false;
    }

    return dy_sound_bank_play(dev, dev->selected);
}

void dy_sound_bank_reset(dy_sound_t *dev)
{
    if (dev == NULL)
    {
        return;
    }

    dev->selected = (uint16_t)DY_SOUND_NONE;
    dev->rotation = 0u;
}

uint16_t dy_sound_bank_selected(const dy_sound_t *dev)
{
    return (dev != NULL) ? dev->selected : (uint16_t)DY_SOUND_NONE;
}

const dy_track_t *dy_sound_bank_find(const dy_sound_t *dev, uint16_t id)
{
    const dy_bank_t *bank = bank_of(dev);

    if ((bank == NULL) || (id == (uint16_t)DY_SOUND_NONE))
    {
        return NULL;
    }

    return find_nth(bank, id, 0u, NULL);
}

uint16_t dy_sound_bank_variants(const dy_sound_t *dev, uint16_t id)
{
    const dy_bank_t *bank = bank_of(dev);
    uint16_t         count = 0u;

    if ((bank == NULL) || (id == (uint16_t)DY_SOUND_NONE))
    {
        return 0u;
    }

    (void)find_nth(bank, id, 0u, &count);
    return count;
}

const char *dy_sound_bank_label(const dy_sound_t *dev, uint16_t id)
{
#if DY_SOUND_LABELS
    const dy_track_t *entry = dy_sound_bank_find(dev, id);

    if ((entry == NULL) || (entry->label == NULL))
    {
        return "";
    }

    return entry->label;
#else
    (void)dev;
    (void)id;
    return "";
#endif
}

bool dy_sound_bank_verify(dy_sound_t *dev, uint16_t *first_missing)
{
    const dy_bank_t *bank = bank_of(dev);
    uint16_t         highest = 0u;
    uint16_t         on_card;
    uint16_t         i;

    if (bank == NULL)
    {
        return false;
    }

    for (i = 0u; i < bank->count; i++)
    {
        if (bank->tracks[i].track > highest)
        {
            highest = bank->tracks[i].track;
        }
    }

    /* A module that says nothing cannot disprove the table. Reporting a fault
     * on silence would make an unwired TX line look like a bad SD card, which
     * is the same confusion this driver exists to avoid. */
    if (!dy_sound_query_song_count(dev, &on_card))
    {
        return true;
    }

    if (highest <= on_card)
    {
        return true;
    }

    if (first_missing != NULL)
    {
        *first_missing = highest;
    }

    return false;
}
