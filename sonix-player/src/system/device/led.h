#ifndef LED_H
#define LED_H

#include <stdbool.h>

// The RGB status LED, driven exactly the way the stock player drives it.
//
// The firmware loads leds_sgm31324_add.ko, which registers a table of preset
// colour patterns with the SGM31324 RGB controller and exposes them as small
// integers on /sys/class/leds/sgm31324-leds/led_pattern. The stock binary
// just writes the pattern number for the state it is in; the numbers below
// come out of its logical->pattern table (FUN_0047ce00) and its callers:
//
//   1   aqua           device on, nothing playing (written at stock startup)
//   2   yellow         PCM at or below 48 kHz
//   3   aqua           PCM above 48 up to 192 kHz
//   4   orange         PCM above 192 kHz
//   5   off            the logical-0 state (all-zero registers)
//   6   red breathing  charging: the stock LED state machine forces this,
//                      last, whenever its charger flag is set; the register
//                      block (blink period, ~50% duty, ramp, current on the
//                      red channel only) is the charging pulse
//   11  white          DSD (the white LED on GPIO PF11, wled_pattern_id=11)
//   8   green          MQA
//   9   blue           MQA Studio
//   10  magenta        MQA rendering
//
// Priority when states overlap, matching the stock state machine exactly:
// charging red > playback colour > idle aqua (the stock code applies the
// charging override after computing the playback colour).
//
// The R1 has no RGB controller: leds_pwm_add.ko registers two plain LEDs,
// /sys/class/leds/red and /sys/class/leds/blue. There every lit pattern
// becomes blue at brightness 50 and off stays off, as its stock player does
// (blue 50 for every state but 0 and 6), and the charging red is the red LED
// with its "breathing" trigger. The same states, priorities and switches
// apply; only the output differs. Which player it is comes from the nodes
// present, the pattern node winning.
//
// Everything degrades to a no-op when the sysfs nodes are missing (the host
// build, or firmware without the module loaded).

// Probes the sysfs nodes and replays the stock startup sequence: red trigger
// released, power-on aqua (pattern 1).
void led_init(void);

// Red breathing while a charge is actually going in, overriding the playback
// colour like the stock player does. Not "a cable is in": a battery at 100%%,
// or held at the configured charge limit, has nothing left to indicate and the
// LED goes back to whatever it would be showing with no cable -- the playback
// colour, a mode's colour, or the idle aqua. The red is an override on top of
// the rest, not a state of its own.
//
// Cheap to call repeatedly: it only writes when the resolved pattern changes.
// But the caller must hand it a settled answer -- the pattern is a ramp
// programmed into the LED controller, and rewriting it restarts the ramp, so a
// reading that flaps comes out as a red that stutters.
void led_set_charging(bool charging);

// Whether a cable is supplying at all, charge finished or not. It does not
// light anything by itself; what it does is keep the darken-in-standby option
// off the LED. Without it a player left plugged in overnight answers the end of
// its charge by going dark -- the red stops having anything to say and the
// standby option, whose window ran out hours ago, takes the LED straight out.
void led_set_on_charger(bool present);

// Picks the pattern for what is playing (shown when not charging). `playing`
// false or rate 0 falls back to the idle aqua. `podcast` true paints the
// purple (pattern 10) instead of the sample-rate colour, and `dsd` true the
// white (pattern 11): a DSD file goes to the DAC over DoP, at a PCM carrier
// rate of 176.4 kHz and up that would otherwise read as hi-res PCM. Cheap to
// call repeatedly: only writes on change.
void led_update_playback(bool playing, int sample_rate, bool podcast, bool dsd);

// Wi-Fi transfer active: pattern 12 (a register added by hand to the driver
// script). Stays lit with the screen off, since it indicates the server is
// running, and overrides the darken-in-standby option.
void led_set_wifi_transfer(bool active);

// Bluetooth receiver mode: pattern 14, blue at full brightness (another
// register added by hand to the driver script). Same argument as the transfer
// above, and the same exemption from the standby option -- nothing plays
// through audio.c in that mode, so led_update_playback() reads a stopped
// player, and the LED went out twenty seconds after the screen did with a
// computer still sending music into the jack.
void led_set_bt_receiver(bool active);

// The codec the receiver is taking in, as bluealsa names it ("" when none). In
// receiver mode it picks a fixed colour, the one the on-screen indicator uses:
// SBC yellow, AAC green, aptX and aptX HD magenta, LDAC cyan; any other keeps
// the BT receiver colour from the LED page. Safe from the receiver thread: it
// only records the codec, and the next led_set_bt_receiver() poll lights it.
void led_set_bt_codec(const char *codec);

// The codec headphones are being fed with ("" or NULL when none). While a
// track plays it takes the place of the sample-rate colour, with the same
// fixed colours as above; charging, podcast and DSD still win over it. From
// the interface thread only.
void led_set_bt_tx_codec(const char *codec);

// DAC mode. Nothing plays through audio.c there, so led_update_playback() sees
// a stopped player and the LED went out twenty seconds after the screen did,
// with the computer still sending music. While the host streams, the rate gets
// the colour a local track of that rate would; while the mode is merely up, the
// LED stays on the idle colour rather than being darkened by the standby
// option -- the same reasoning as the transfer above.
// `dsd` true is a DoP stream: white, whatever the carrier rate, as in
// led_update_playback().
void led_set_dac(bool active, bool streaming, int sample_rate, bool dsd);

// The LED master switch. Off keeps the LED dark whatever else is
// going on -- with one exception, the same one the stock player makes: a
// charge in progress still shows its red, because a device that looks dead
// on the charger is worse than a lit LED. On (the default) restores the
// normal behaviour, including the standby option below.
void led_set_enabled(bool enabled);

// The darken-in-standby option: with it on, the LED goes dark after
// twenty seconds in standby (screen off) with nothing playing. A charge in
// progress is never darkened by it -- the red keeps breathing -- and any
// playback or wake lights the LED right back up. Re-evaluated on the same
// polls as the rest, so the switch-off lands within a few seconds.
void led_set_idle_off(bool enabled);

// Screen state, fed by power.c: the standby option only ever darkens the LED
// while the screen is off. Waking restarts the idle window.
void led_set_standby(bool standby);

// The states whose colour can be chosen (Power > LED colours), each mapped to
// one of the controller's patterns ([led] colour_<state> in the config). The
// index is what gets saved, so new states go at the end: the PCM ranges were
// three (up to 48, up to 192, above 192 kHz) and became five, the two added
// after BT_RX.
typedef enum {
	LED_STATE_IDLE,
	LED_STATE_PCM_48K,	// 44.1-48 kHz
	LED_STATE_PCM_192K, // 176.4-192 kHz
	LED_STATE_PCM_384K, // 352.8-384 kHz
	LED_STATE_DSD,
	LED_STATE_PODCAST,
	LED_STATE_CHARGING,
	LED_STATE_WIFI,
	LED_STATE_BT_RX,
	LED_STATE_PCM_96K,	// 88.2-96 kHz
	LED_STATE_PCM_768K, // above 384 kHz
	LED_STATE_COUNT,
} led_state_t;

void led_set_state_pattern(led_state_t state, int pattern);
int led_state_pattern(led_state_t state);

// Lights `pattern` now, whatever the player is doing, until led_preview_end().
void led_preview(int pattern);
void led_preview_end(void);

#endif /* LED_H */
