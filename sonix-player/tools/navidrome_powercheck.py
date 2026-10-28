#!/usr/bin/env python3
"""Compile production power predicates and activity admission; execute no power action."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

PLAYER = Path(__file__).resolve().parents[1]
source = (PLAYER / "src/system/device/power.c").read_text()


def function(name):
    start = re.search(r"static (?:bool|void) " + name + r"\(", source).start()
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


policy = function("wifi_in_use") + "\n" + function("player_is_busy")
# Existing unrelated policy inputs are neutral. Production predicate expressions
# themselves are compiled verbatim, so omission of activity fails these checks.
calls = set(re.findall(r"\b([a-z][a-z_]+)\(", policy))
neutral = calls - {"wifi_in_use", "player_is_busy", "work_activity_network", "work_activity_busy", "power_tick_ms"}
code = '#include "src/system/device/workactivity.h"\n#include <assert.h>\n#include <stdint.h>\n#include <pthread.h>\n#include <stdatomic.h>\n#include <stdio.h>\n'
code += "\n".join(f"#define {name}(...) 0" for name in sorted(neutral))
code += "\n#define power_tick_ms() 250u\n" + policy
code += r'''
static atomic_bool owner_ready, release_owner;
static void *owner(void *unused) {
    (void)unused;
    work_activity_t token = {0}; assert(work_activity_begin(&token,WORK_STORAGE));
    atomic_store(&owner_ready,true);
    while (!atomic_load(&release_owner)) { }
    work_activity_end(&token); return NULL;
}
int main(void) {
    assert(!wifi_in_use() && !player_is_busy(false,1000000));
    for (unsigned mask=1;mask<8;mask++) {
        work_activity_t old={0}, current={0};
        assert(work_activity_begin(&old,mask));
        assert(player_is_busy(false,1000000));
        assert(wifi_in_use()==((mask & WORK_NETWORK)!=0));
        assert(!work_activity_suspend_begin());
        assert(work_activity_begin(&current,mask));
        work_activity_end(&old); work_activity_end(&old);
        assert(work_activity_busy()); // late/idempotent old end cannot clear current
        work_activity_end(&current);
        assert(!work_activity_busy());
    }
    work_activity_t tokens[1024]={0};
    for (int i=0;i<1023;i++) assert(work_activity_begin(&tokens[i],7));
    assert(!work_activity_begin(&tokens[1023],7));
    for (int i=0;i<1023;i++) work_activity_end(&tokens[i]);
    assert(!work_activity_busy());
    pthread_t thread; assert(!pthread_create(&thread,NULL,owner,NULL));
    while (!atomic_load(&owner_ready)) { }
    for (int i=0;i<10000;i++) assert(!work_activity_suspend_begin());
    atomic_store(&release_owner,true); pthread_join(thread,NULL);
    assert(work_activity_suspend_begin());
    for (unsigned mask=1;mask<8;mask++) {
        work_activity_t rejected={0}; assert(!work_activity_begin(&rejected,mask));
        assert(!rejected.mask);
    }
    work_activity_suspend_end();
    assert(!work_activity_busy() && !wifi_in_use() && !player_is_busy(false,1000000));
    puts("Production power predicates, ownership/capacity/late releases and suspend admission: passed; no power action executed.");
}
'''
assert "work_activity_suspend_begin()" in function("suspend_if_idle")
assert "work_activity_suspend_end()" in function("suspend_if_idle")
with tempfile.TemporaryDirectory(prefix="sonix-power-policy-") as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(code)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I.",
        str(path / "check.c"), "src/system/device/workactivity.c", "-pthread", "-o", str(path / "check"),
    ]
    command[1:1] = shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
    subprocess.run(command, cwd=PLAYER, check=True)
    subprocess.run([str(path / "check")], check=True)

# Exercise the actual standby registry against native LVGL timers. These are
# lifetime-owned timers in production; the fixture clears the registry at exit.
registry_start = source.index("#define STANDBY_TIMERS_MAX")
registry_end = source.index("// LVGL's input-read timers", registry_start)
timers = '#include "lvgl/lvgl.h"\n#include "lvgl/src/misc/lv_timer_private.h"\n#include <assert.h>\n#include <stdio.h>\n'
timers += source[registry_start:registry_end] + function("standby_timers_set_paused")
timers += r'''
lv_font_t font_ui_14;
void frameprof_begin(const char *slot) { (void)slot; }
void frameprof_end(const char *slot) { (void)slot; }
bool frameprof_timing(void) { return false; }
unsigned long long frameprof_now_us(void) { return 0; }
void frameprof_timer_ran(void *cb, unsigned us) { (void)cb; (void)us; }
void frameprof_event_ran(void *cb, unsigned us) { (void)cb; (void)us; }
static void tick(lv_timer_t *timer) { (void)timer; }
int main(void) {
    lv_init();
    lv_timer_t *visual=lv_timer_create(tick,100,NULL), *inactive=lv_timer_create(tick,200,NULL), *control=lv_timer_create(tick,250,NULL);
    lv_timer_pause(inactive);
    power_pause_in_standby(visual); power_pause_in_standby(inactive); power_slow_in_standby(control,1000);
    power_pause_in_standby(visual); assert(g_standby_timer_count==3);
    standby_timers_set_paused(true);
    assert(lv_timer_get_paused(visual) && lv_timer_get_paused(inactive));
    assert(!lv_timer_get_paused(control) && control->period==1000);
    lv_timer_pause(control); // completion settles while the panel is dark
    standby_timers_set_paused(false);
    assert(!lv_timer_get_paused(visual) && lv_timer_get_paused(inactive));
    assert(lv_timer_get_paused(control) && control->period==250);
    for (int i=g_standby_timer_count;i<STANDBY_TIMERS_MAX;i++) power_slow_in_standby(lv_timer_create(tick,1000,NULL),2000);
    assert(g_standby_timer_count==STANDBY_TIMERS_MAX);
    standby_timers_set_paused(true); standby_timers_set_paused(false);
    g_standby_timer_count=0;
    lv_deinit();
    puts("Production standby registry: capacity, duplicate registration, visual pause/catch-up and settled control timers passed.");
}
'''
objects = sorted((PLAYER / "build_host/lvgl").rglob("*.o"))
if not objects:
    raise SystemExit("Requires existing host LVGL objects; standby registry check did not run.")
with tempfile.TemporaryDirectory(prefix="sonix-power-timers-") as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(timers)
    command = shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections", "-DHOST_BUILD", "-DLV_CONF_INCLUDE_SIMPLE", "-I.", "-Ilvgl"]
    command += shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", ""))
    command += [str(path / "check.c")] + [str(p) for p in objects]
    command += shlex.split(subprocess.check_output(["pkg-config", "--libs", "freetype2", "sdl2"], text=True))
    command += ["-Wl,--gc-sections", "-pthread", "-lm", "-o", str(path / "check")]
    subprocess.run(command, cwd=PLAYER, check=True)
    subprocess.run([str(path / "check")], check=True)
