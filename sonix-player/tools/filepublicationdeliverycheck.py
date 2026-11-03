#!/usr/bin/env python3
"""Production GUI completion handlers, with inert rendering edges; no display.

Checks visit, card and navigation rejection using the exact handler bodies.
The separate consumercheck exercises real filesystem listing/mutations.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

player = Path(__file__).resolve().parents[1]
def function(path, signature):
    source = (player / path).read_text()
    start = source.index(signature + ' {')
    body = source.index('{', start)
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
typedef struct { int unused; } lv_obj_t;
static lv_obj_t objects[4];
static lv_obj_t *details_screen = objects, *details_card = objects + 1;
static lv_obj_t *filespage_screen = objects + 2, *file_list = objects + 3;
static lv_obj_t *active, *empty_label;
static bool valid_screen = true;
static uint64_t epoch = 9, entries_epoch;
static unsigned details_visit = 4, folder_visit = 4;
static char details_path[512] = "/card/song", path[512] = "/card/album";
static int cleaned, painted, freed, rows;
static bool rebuilding;
static int entry_count, skipped;
static void *entries;
static pthread_mutex_t folder_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
 char path[512], codec[16]; unsigned visit; uint64_t card_epoch;
 int tags, bits, channels, kbps, dsd; double rate, duration;
 bool lossy, ok, size_known; struct stat size;
} details_job_t;
typedef struct {
 char path[512]; unsigned visit; uint64_t card_epoch;
 void *entries; int count, skipped; bool failed;
} folder_job_t;
static folder_job_t *folder_result;
static bool fp_card_local_current(uint64_t candidate) { return candidate == epoch; }
static bool lv_obj_is_valid(lv_obj_t *obj) { return obj && valid_screen; }
static lv_obj_t *lv_screen_active(void) { return active; }
static void lv_obj_clean(lv_obj_t *obj) { assert(obj); cleaned++; }
#define details_fill(...) (painted++)
#define details_add_row(...) (painted++)
#define tr(s) (s)
#define lv_label_create(parent) (parent)
#define lv_label_set_text(...) ((void)0)
#define lv_obj_set_style_text_align(...) ((void)0)
#define lv_obj_set_width(...) ((void)0)
#define lv_obj_add_style(...) ((void)0)
#define lv_obj_set_style_text_font(...) ((void)0)
#define lv_obj_set_style_pad_top(...) ((void)0)
#define ROWS_FIRST 20
static void entries_free(void) { free(entries); entries = NULL; }
static void rows_add(int n) { assert(n == ROWS_FIRST); rows++; }
static void rows_fill(void) { rows++; }
static void folder_free(folder_job_t *job) { if (job) { free(job->entries); free(job); freed++; } }
'''
code += function('src/gui/nowplaying/trackmenu.c', 'static void details_done(void *data)')
code += function('src/gui/library/filespage.c', 'static void folder_deliver(void *unused)')
code += r'''
int main(void) {
 for (int variant = 0; variant < 6; variant++) {
  details_job_t *job = calloc(1, sizeof(*job)); assert(job);
  strcpy(job->path, details_path); job->visit=4; job->card_epoch=9; job->ok=true;
  active = details_screen; valid_screen=true;
  if (variant==0) job->visit--;
  if (variant==1) job->card_epoch--;
  if (variant==2) active=filespage_screen;
  if (variant==3) valid_screen=false;
  if (variant==4) strcpy(job->path,"/card/old");
  details_done(job);
  assert(cleaned==(variant==5) && painted==(variant==5));
 }
 cleaned=0;
 for (int variant = 0; variant < 6; variant++) {
  folder_result = calloc(1, sizeof(*folder_result)); assert(folder_result);
  strcpy(folder_result->path,path); folder_result->visit=4; folder_result->card_epoch=9;
  folder_result->entries=calloc(1,8); folder_result->count=2;
  active=filespage_screen; valid_screen=true;
  if (variant==0) folder_result->visit--;
  if (variant==1) folder_result->card_epoch--;
  if (variant==2) active=details_screen;
  if (variant==3) valid_screen=false;
  if (variant==4) strcpy(folder_result->path,"/card/old");
  folder_deliver(NULL);
  assert(!folder_result && freed==variant+1);
  assert(cleaned==(variant==5) && rows==2*(variant==5));
 }
 assert(entries_epoch==epoch && entry_count==2 && !rebuilding);
 entries_free();
 puts("PASS production Track Details and File Explorer delivery: retired visits, replaced cards, hidden/deleted pages and changed paths reject; current results render and release ownership");
}
'''
with tempfile.TemporaryDirectory(prefix='sonix-publication-delivery-') as tmp:
    root = Path(tmp)
    source = root / 'check.c'
    source.write_text(code)
    binary = root / 'check'
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g'] + shlex.split(os.environ.get('NAVIDROME_CHECK_FLAGS','')) + [str(source),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
