#ifndef BATTERYSTATS_H
#define BATTERYSTATS_H

#include <stdbool.h>
#include <stdint.h>

// Where the battery went over the last day, hour by hour: how long the screen
// was on, dark with the SoC awake (standby: the LED still lit, the panel wakes
// at once) and suspended to RAM (sleep), what played and from where, and how
// much of the battery each of those took.
//
// The battery says how full it is and nothing about who is drawing from it,
// so the share of each item is an estimate. The total is the gauge's own:
// every drop of the level is split among what was active since the drop
// before, in proportion to a fixed model of what each part draws (RATE_* in
// batterystats.c). Only the split is modelled, which keeps it from swinging
// from one day to the next.

typedef enum {
	BS_LOCAL, // files on the card, audiobooks included
	BS_BT_RECEIVER,
	BS_USB_DAC,
	BS_RADIO,
	BS_QOBUZ,
	BS_TIDAL,
	BS_PODCAST,
	BS_AIRPLAY,
	BS_DLNA,
	BS_SCREEN,	// screen on
	BS_STANDBY, // screen dark, SoC awake
	BS_SLEEP,	// suspended to RAM
	BS_BT_OUT,	// any of the sources above going out to Bluetooth headphones
	BS_ITEM_COUNT
} bs_item_t;

#define BS_SOURCE_COUNT BS_SCREEN // the items before it are where the music came from
#define BS_HOURS 24
#define BS_HOUR_MS 3600000LL

typedef struct {
	int32_t hour;				  // time() / 3600 of the hour this holds; 0 = nothing recorded
	uint32_t ms[BS_ITEM_COUNT];	  // how long each item was active
	float used[BS_ITEM_COUNT];	  // % of the battery, from drops already measured
	float pending[BS_ITEM_COUNT]; // % by the model alone, since the last drop
	int8_t level;				  // the last battery % read in the hour, -1 for none
	bool cable;					  // the device was on external power at some point
} batterystats_hour_t;

// Reads back what was saved. Once, before the first tick.
void batterystats_init(void);

// Counts the time since the last call. From the power tick, on the interface
// thread, like everything else here.
void batterystats_tick(bool screen_on);

// Deletes the saved counts and writes nothing more: the factory reset, on its
// way to the reboot.
void batterystats_forget(void);

// Writes the counts out. The tick does it every ten minutes, and the shutdown,
// the restart and the firmware update on their way out. A suspend needs none:
// the time asleep is only known, and counted, at the first tick after it.
void batterystats_save(void);

// The hour `ago` hours before the current one (0 .. BS_HOURS-1), or NULL if
// nothing was recorded in it.
const batterystats_hour_t *batterystats_hour(int ago);

// Wall-clock ms from which the counting has been running: an hour with nothing
// recorded after it was spent switched off, one before it is simply unknown.
int64_t batterystats_since_ms(void);

#endif /* BATTERYSTATS_H */
