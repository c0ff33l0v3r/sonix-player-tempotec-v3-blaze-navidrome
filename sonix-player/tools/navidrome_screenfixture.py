"""Compile the actual screen transitions with inert display/hardware edges.

Used by the download worker fixture. No power, sysfs, radio or LVGL operation
is executed. The independent powercheck exercises native LVGL timer behavior.
"""


def screen_fixture(player):
    source = (player / 'src/system/device/power.c').read_text()
    def function(name):
        start = source.index('void ' + name + '(void)')
        body = source.index('{', start)
        depth, end = 1, body + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]
    code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
typedef void lv_obj_t;
typedef void lv_timer_t;
static bool g_screen_on = true, g_double_tap_wake, g_panel_lit_dark;
static bool g_mem_blocked_this_dark;
static unsigned g_mem_instant_wakes, g_screen_off_at, g_mem_active_ms;
static long g_hw_brightness;
static uint64_t g_last_active_boot_ms, wake_t_enter;
static struct { const char *brightness_path; long brightness; } g_cfg = {0, 50};
static bool (*g_wake_hook)(void);
static void *g_disp __attribute__((unused)), *g_refr_timer;
static bool visual_paused, inputs_enabled = true;
#define BRIGHTNESS_MIN 1
#define PROF_WAKE_UNBLANK "unblank"
#define PROF_WAKE_SETTLE "settle"
#define PROF_WAKE_DRAW "draw"
#define PROF_WAKE_FADE "fade"
#define set_indevs_enabled(on) (inputs_enabled = (on))
#define standby_timers_set_paused(paused) (visual_paused = (paused))
#define lv_tick_get() 1234u
#define boottime_ms() 1234u
#define wake_us() 1234000u
#define lv_anim_get_timer() ((lv_timer_t *)0)
#define lv_screen_active() ((lv_obj_t *)0)
#define lv_layer_top() ((lv_obj_t *)0)
#define config_get_int(section, key, fallback) (fallback)
'''
    inert = '''waveform_set_screen_on power_set_double_tap_wake fade_to
write_long_to_file screen_power lv_obj_invalidate lv_refr_now display_mirror_page
 display_show_black lv_timer_pause indev_timers_set_paused led_set_standby
 clock_remember drop_presses_made_during_transition rtc_alarm_disarm unpark_radios
 clock_resync display_black_wake frameprof_begin frameprof_end backlight_write
 lv_timer_resume lv_anim_refr_now display_wake_begin display_wake_end
 display_wait_flip lv_display_trigger_activity'''.split()
    code += '\n'.join(f'#define {name}(...) ((void)0)' for name in inert) + '\n'
    code += function('power_screen_off') + '\n' + function('power_screen_on')
    code += '''
void fixture_screen_off(void) {
    power_screen_off();
    assert(!g_screen_on && visual_paused && !inputs_enabled);
}
void fixture_screen_on(void) {
    power_screen_on();
    assert(g_screen_on && !visual_paused && inputs_enabled);
}
'''
    return code
