#!/usr/bin/env python3
"""Reproduce review probes using committed production sources and bundled SQLite.

No full build, sockets, real card data, credentials or power actions are used.
"""
import json
import os
from pathlib import Path
import shlex
import sys
import subprocess
import tempfile

PLAYER = Path(__file__).resolve().parents[1]
REPO = PLAYER.parent
BASELINE = "91a90cb"
COMMON = r'''
#include "src/system/db/sqlite3.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
static double milliseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
'''
STATE = r'''
static atomic_bool entered;
static int delayed_exec(sqlite3 *db, const char *sql,
        int (*cb)(void *, int, char **, char **), void *arg, char **err) {
    if (!strcmp(sql, "COMMIT")) { atomic_store(&entered, true); usleep(300000); }
    return sqlite3_exec(db, sql, cb, arg, err);
}
#define sqlite3_exec delayed_exec
#include "probe_source.c"
#undef sqlite3_exec
static sqlite3 *database;
static void *commit_probe(void *unused) {
    (void)unused;
    nd_context_t c = {.epoch=1, .card_epoch=1};
    assert(navidrome_context_commit(&c, database));
    return NULL;
}
int main(void) {
    g.unlocked = true; g.local_epoch = g.card_epoch = 1;
    assert(sqlite3_open(":memory:", &database) == SQLITE_OK);
    assert(sqlite3_exec(database, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    pthread_t worker; assert(!pthread_create(&worker, NULL, commit_probe, NULL));
    while (!atomic_load(&entered)) usleep(100);
    double start = milliseconds(); navidrome_snapshot_t snapshot;
    navidrome_snapshot(&snapshot);
    double blocked = milliseconds() - start;
    pthread_join(worker, NULL); sqlite3_close(database);
    printf("{\"snapshot_blocked_ms\":%.3f}\n", blocked);
}
'''
CACHE = r'''
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
static unsigned statuses, opens, spaces;
static int counted_space(const char *p, struct statvfs *s) { spaces++; return statvfs(p,s); }
static int counted_stat(const char *p, struct stat *s) { statuses++; return lstat(p,s); }
static int counted_open(const char *p, int f, ...) {
    mode_t mode = 0;
    if (f & O_CREAT) { va_list args; va_start(args,f); mode=va_arg(args,int); va_end(args); }
    opens++; return open(p,f,mode);
}
#define statvfs(...) counted_space(__VA_ARGS__)
#define lstat counted_stat
#define open counted_open
#include "probe_source.c"
#undef statvfs
#undef lstat
#undef open
bool growfile_is_growing(const char *p) { (void)p; return false; }
unsigned nd_cache_epoch(void) { return 1; }
int main(void) {
    char root[]="/tmp/sonix-workprobe-cache-XXXXXX"; assert(mkdtemp(root));
    char dir[512], path[512]; snprintf(dir,sizeof(dir),"%s/%032d",root,0); assert(!mkdir(dir,0700));
    for (int i=0;i<200;i++) {
        snprintf(path,sizeof(path),"%s/%032x.flac",dir,i);
        int fd=open(path,O_WRONLY|O_CREAT,0600); assert(fd>=0); assert(write(fd,"fLaC",4)==4); close(fd);
        complete_t record={.version=1,.bytes=4}; strcpy(record.extension,".flac");
        snprintf(path,sizeof(path),"%s/%032x.complete",dir,i);
        fd=open(path,O_WRONLY|O_CREAT,0600); assert(fd>=0);
        assert(write(fd,&record,sizeof(record))==sizeof(record)); close(fd);
    }
    statuses=opens=0;
    for (int i=0;i<160;i++) assert(room(root,65536));
    printf("{\"cache_status_checks\":%u,\"cache_marker_opens\":%u,\"cache_free_space_checks\":%u}\n",statuses,opens,spaces);
#ifdef PROBE_CURRENT
    // Sparse originals force more than four candidate batches without using
    // physical card capacity. Actual completed_file/eviction paths are used.
    for (int i=0;i<200;i++) {
        snprintf(path,sizeof(path),"%s/%032x.flac",dir,i);
        int fd=open(path,O_WRONLY); assert(fd>=0 && !ftruncate(fd,8*1024*1024)); close(fd);
        complete_t record={.version=1,.bytes=8*1024*1024}; strcpy(record.extension,".flac");
        snprintf(path,sizeof(path),"%s/%032x.complete",dir,i);
        fd=open(path,O_WRONLY|O_TRUNC); assert(fd>=0 && write(fd,&record,sizeof(record))==sizeof(record)); close(fd);
    }
    for (int i=0;i<3;i++) snprintf(protected_paths[i],sizeof(protected_paths[i]),"%s/%032x.flac",dir,i);
    snprintf(copy_path,sizeof(copy_path),"%s/%032x.flac",dir,3);
    audio_space_t eviction={0}; snprintf(eviction.current,sizeof(eviction.current),"%s/%032x.flac",dir,4);
    statuses=opens=spaces=0;
    assert(room(&eviction,root,65536+sizeof(complete_t)));
    int removed=0;
    for (int i=0;i<200;i++) {
        snprintf(path,sizeof(path),"%s/%032x.flac",dir,i);
        if (access(path,F_OK)) { assert(i>=5); removed++; }
    }
    assert(removed>64 && statuses<8*801);
    fprintf(stderr,"Cache eviction: %d victims, %u status checks, bounded candidate refills; current/next-two/copy protected\n",removed,statuses);
#endif
    for (int i=0;i<200;i++) {
        snprintf(path,sizeof(path),"%s/%032x.flac",dir,i); assert(!unlink(path) || errno==ENOENT);
        snprintf(path,sizeof(path),"%s/%032x.complete",dir,i); assert(!unlink(path) || errno==ENOENT);
    }
    assert(!rmdir(dir)); assert(!rmdir(root));
}
'''
QUERY = r'''
static unsigned long long steps;
#ifdef PROBE_CURRENT
static bool fail_allocation;
static unsigned fault_pages;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t bytes) {
    if (fail_allocation) { fail_allocation=false; return NULL; }
    return __real_malloc(bytes);
}
#endif
static int counted_step(sqlite3_stmt *statement) {
#ifdef PROBE_CURRENT
    if (fault_pages && !strncmp(sqlite3_sql(statement),"INSERT INTO position",20) && !--fault_pages) return SQLITE_FULL;
#endif
    int rc = sqlite3_step(statement);
    steps += sqlite3_stmt_status(statement,SQLITE_STMTSTATUS_VM_STEP,1); return rc;
}
#define sqlite3_step counted_step
#include "probe_source.c"
#undef sqlite3_step
#ifdef PROBE_CURRENT
static int64_t compare_generation=1;
static bool cancelled(void *unused) { (void)unused; return false; }
static bool reject_order(void *unused, const char *temp, const char *path) {
    (void)unused; (void)temp; (void)path; return false;
}
static void compare_order(nd_store_t *s, nd_query_t *q, int offset, bool write) {
    nd_row_t expected[ND_WINDOW], actual[ND_WINDOW]; int ne,na,te,ta;
    assert(nd_store_query(s,compare_generation,q,offset,expected,&ne,&te));
    assert(nd_store_query_cached(s,compare_generation,q,offset,actual,&na,&ta,write));
    assert(ne==na && te==ta && !memcmp(expected,actual,ne*sizeof(*actual)));
}
#endif
int main(int argc, char **argv) {
    assert(argc==2); nd_store_t store;
    assert(nd_store_open(&store,argv[1],"fixture",true,false));
    assert(sql(&store,"INSERT INTO snapshot VALUES(1,'fixture',1,0); BEGIN;"
        "WITH RECURSIVE n(v) AS (VALUES(0) UNION ALL SELECT v+1 FROM n WHERE v<1699) "
        "INSERT INTO entity SELECT 1,0,printf('a%04d',v),printf('Album %04d',v),'Artist','','',0,0,0,printf('album %04d',v) FROM n;"
        "WITH RECURSIVE n(v) AS (VALUES(0) UNION ALL SELECT v+1 FROM n WHERE v<16999) "
        "INSERT INTO entity SELECT 1,1,printf('t%05d',v),printf('Track %05d',v),'Artist',printf('a%04d',v/10),'',1,1,v%10,printf('track %05d',v) FROM n; COMMIT;"));
    nd_row_t rows[ND_WINDOW]; int count,total;
    nd_query_t q={.kind=ND_TRACKS};
    assert(nd_store_query(&store,1,&q,0,rows,&count,&total));
    steps=0; assert(nd_store_query(&store,1,&q,0,rows,&count,&total));
    unsigned long long title=steps;
    steps=0; q.order=ND_ORDER_ALBUM;
    assert(nd_store_query(&store,1,&q,0,rows,&count,&total));
    printf("{\"query_title_vm_steps\":%llu,\"query_album_vm_steps\":%llu,\"tracks\":%d,\"sqlite\":\"%s\"}\n",title,steps,total,sqlite3_libversion());
#ifdef PROBE_CURRENT
    steps=0;
    assert(nd_store_query_cached(&store,1,&q,0,rows,&count,&total,true));
    unsigned long long build=steps, baseline_work=0, cached_work=0;
    for (int i=0;i<100;i++) {
        int offset=i%3==0 ? 0 : i%3==1 ? 8500 : 16984;
        nd_row_t expected[ND_WINDOW]; int expected_count, expected_total;
        steps=0; assert(nd_store_query(&store,1,&q,offset,expected,&expected_count,&expected_total));
        baseline_work+=steps;
        nd_store_close(&store);
        assert(nd_store_open(&store,argv[1],"fixture",false,false));
        steps=0; assert(nd_store_query_cached(&store,1,&q,offset,rows,&count,&total,false));
        cached_work+=steps;
        assert(expected_count==count && expected_total==total && !memcmp(expected,rows,count*sizeof(*rows)));
    }
    assert(cached_work*10<baseline_work);
    fprintf(stderr,"Grouped order build=%llu; 100 reopened windows baseline=%llu cached=%llu VM steps\n",build,baseline_work,cached_work);
    nd_store_close(&store);
    assert(nd_store_open(&store,argv[1],"fixture",true,false));
    assert(sql(&store,"UPDATE entity SET artist=CASE rowid%4 WHEN 0 THEN 'Étoile' WHEN 1 THEN 'The Band' WHEN 2 THEN '中文' ELSE '' END;"
        "UPDATE entity SET album='missing' WHERE kind=1 AND rowid%5=0;"
        "INSERT INTO album_credit SELECT gen,rowid,CASE rowid%3 WHEN 0 THEN 'Explicit Ωμέγα' WHEN 1 THEN 'The Beatles' ELSE '' END FROM entity WHERE kind=0;"
        "INSERT INTO track_navigation SELECT gen,id,artist FROM entity WHERE kind=1;"));
    store.view=-1;
    for (int mode=ND_ORDER_ALBUM;mode<=ND_ORDER_ARTIST_DESC;mode++) {
        q.order=mode;
        for (int i=0;i<3;i++) compare_order(&store,&q,i==0?0:i==1?8500:16984,true);
        nd_store_close(&store);
        assert(nd_store_open(&store,argv[1],"fixture",false,false));
        steps=0; assert(nd_store_query_cached(&store,1,&q,8500,rows,&count,&total,false));
        assert(steps<5000); // exact sealed reuse, never hidden SQL fallback
    }
    char artifact[1200]; snprintf(artifact,sizeof(artifact),"%s",argv[1]);
    strcpy(strrchr(artifact,'/')+1,"browse-order.db");
    struct stat st; assert(!lstat(artifact,&st) && st.st_size<=ORDER_PAGES*4096LL);
    sqlite3 *index=NULL; assert(order_open(artifact,&index,false));
    sqlite3_stmt *cap=NULL; assert(sqlite3_prepare_v2(index,"PRAGMA max_page_count",-1,&cap,NULL)==SQLITE_OK);
    assert(sqlite3_step(cap)==SQLITE_ROW && sqlite3_column_int(cap,0)==ORDER_PAGES);
    sqlite3_finalize(cap); sqlite3_close(index);
    // Missing/corrupt/oversized/locked sidecars must preserve exact fallback.
    assert(!unlink(artifact)); compare_order(&store,&q,16984,false);
    int fd=open(artifact,O_CREAT|O_WRONLY,0600); assert(fd>=0 && write(fd,"corrupt",7)==7); close(fd);
    compare_order(&store,&q,8500,false);
    fd=open(artifact,O_WRONLY); assert(fd>=0 && !ftruncate(fd,ORDER_PAGES*4096LL+1)); close(fd);
    compare_order(&store,&q,0,false); assert(!unlink(artifact));
    nd_row_t expected[ND_WINDOW]; int expected_count,expected_total;
    assert(nd_store_query(&store,1,&q,0,expected,&expected_count,&expected_total));
    fail_allocation=true;
    assert(nd_store_query_cached(&store,1,&q,0,rows,&count,&total,true) && !fail_allocation);
    assert(count==expected_count && total==expected_total && !memcmp(rows,expected,count*sizeof(*rows)));
    if (!lstat(artifact,&st)) assert(!unlink(artifact)); // a failed read-open may rebuild successfully
    fault_pages=5; compare_order(&store,&q,0,true); assert(!fault_pages && lstat(artifact,&st));
    store.publish_order=reject_order; compare_order(&store,&q,0,true);
    assert(lstat(artifact,&st) && errno==ENOENT); store.publish_order=NULL;
    store.current=cancelled;
    assert(!nd_store_query_cached(&store,1,&q,0,rows,&count,&total,true));
    assert(lstat(artifact,&st) && errno==ENOENT); store.current=NULL;
    char guard[1230]; snprintf(guard,sizeof(guard),"%s.lock",artifact);
    fd=open(guard,O_RDWR); assert(fd>=0 && !flock(fd,LOCK_EX|LOCK_NB));
    compare_order(&store,&q,8500,true); assert(lstat(artifact,&st)); close(fd);
    // Favorites retained view, including explicit stars and changed row handles.
    nd_store_close(&store); assert(nd_store_open_source(&store,argv[1],"fixture",true,true,true));
    assert(sql(&store,"DELETE FROM entity; DELETE FROM snapshot; INSERT INTO snapshot VALUES(1,'fixture',1,0);"
        "WITH RECURSIVE n(v) AS (VALUES(0) UNION ALL SELECT v+1 FROM n WHERE v<16999) "
        "INSERT INTO entity SELECT 1,1,printf('t%05d',v),printf('Track %05d',v),CASE v%3 WHEN 0 THEN 'Étoile' WHEN 1 THEN 'The Band' ELSE '' END,'a','',1,1,v%10,printf('track %05d',v) FROM n;"
        "INSERT INTO favorite_song SELECT 1,id,1 FROM entity WHERE kind=1;"
        "INSERT INTO entity VALUES(1,0,'a','Album','Artist','','',0,0,0,'album');"));
    q.favorites=true;
    for (int mode=ND_ORDER_ALBUM;mode<=ND_ORDER_ARTIST_DESC;mode++) {
        q.order=mode;
        for (int i=0;i<3;i++) compare_order(&store,&q,i==0?0:i==1?8500:16984,true);
    }
    assert(sql(&store,"INSERT INTO snapshot VALUES(2,'fixture',1,0); INSERT INTO local_view VALUES(2,1);"
        "INSERT INTO local_change(seq,scope,kind,id,title,artist,album,cover,duration,disc,track,sortkey,starred,details) VALUES(2,'fixture',1,'t00000','Changed','Ωμέγα','a','',1,1,1,'changed',0,0);"));
    compare_generation=2;
    for (int mode=ND_ORDER_ALBUM;mode<=ND_ORDER_ARTIST_DESC;mode++) {
        q.order=mode;
        for (int i=0;i<3;i++) compare_order(&store,&q,i==0?0:i==1?8500:16984,true);
    }
    unsigned long long favorite_baseline=0,favorite_cached=0;
    for (int i=0;i<100;i++) {
        int offset=i%3==0?0:i%3==1?8500:16983;
        nd_row_t expected[ND_WINDOW]; int ne,te;
        steps=0; assert(nd_store_query(&store,2,&q,offset,expected,&ne,&te)); favorite_baseline+=steps;
        nd_store_close(&store); assert(nd_store_open_source(&store,argv[1],"fixture",false,false,true));
        steps=0; assert(nd_store_query_cached(&store,2,&q,offset,rows,&count,&total,false)); favorite_cached+=steps;
        assert(ne==count && te==total && !memcmp(rows,expected,count*sizeof(*rows)));
    }
    assert(favorite_cached*10<favorite_baseline);
    fprintf(stderr,"Changed Favorites: 100 reopened windows baseline=%llu cached=%llu VM steps\n",favorite_baseline,favorite_cached);
    fprintf(stderr,"Grouped-order equivalence: all six directions, Unicode/missing credits, Favorites, readonly reuse, cap/reopen, cancellation, allocation/FULL/lock/publication/corruption faults and changed Favorites views passed\n");
#endif
    nd_store_close(&store);
}
'''


def main():
    current = "--current" in sys.argv
    result = {"revision": "worktree" if current else BASELINE}
    with tempfile.TemporaryDirectory(prefix="sonix-workprobe-") as tmp:
        tmp = Path(tmp)
        sqlite = PLAYER / "build_host/system/db/sqlite3_impl.o"
        if not sqlite.exists():
            sqlite = PLAYER / "src/system/db/sqlite3_impl.c"
        probes = [
            ("state", "navidrome.c", STATE),
            ("cache", "navidromeaudio.c", CACHE.replace(
                'for (int i=0;i<160;i++) assert(room(root,65536));',
                '''audio_space_t space={0}; for (int i=0;i<160;i++) { assert(room(&space,root,(int64_t)(i+1)*65536+sizeof(complete_t))); space.written=(int64_t)(i+1)*65536; }
    unsigned original_statuses=statuses, original_opens=opens, original_spaces=spaces;
    statuses=opens=spaces=0; memset(&space,0,sizeof(space));
    for (int i=0;i<2560;i++) { assert(room(&space,root,(int64_t)(i+1)*4096+sizeof(complete_t))); space.written=(int64_t)(i+1)*4096; }
    assert(statuses==original_statuses && opens==original_opens && spaces==original_spaces);''')),
        ] if current else [
            ("state", "navidrome.c", STATE),
            ("cache", "navidromeaudio.c", CACHE),
            ("query", "navidromestore.c", QUERY),
        ]
        if current and "--query-work" in sys.argv:
            probes.append(("query", "navidromestore.c", QUERY))
        for name, source, code in probes:
            production = (PLAYER / "src/system/streaming" / source).read_bytes() if current else subprocess.check_output(
                ["git", "show", f"{BASELINE}:sonix-player/src/system/streaming/{source}"], cwd=REPO
            )
            (tmp / "probe_source.c").write_bytes(production)
            (tmp / "probe.c").write_text(COMMON + code)
            command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("NAVIDROME_CHECK_FLAGS", "")) + [
                "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                "-DHOST_BUILD", "-DLV_CONF_INCLUDE_SIMPLE", "-I.", "-Ilvgl",
                "-Isrc/system/streaming", str(tmp / "probe.c"), str(sqlite), "src/system/device/workactivity.c",
            ]
            if current: command += ["-DPROBE_CURRENT"]
            if name == "query":
                command += ["src/system/library/library.c"]
                if current: command += ["-Wl,--wrap=malloc"]
            command += ["-Wl,--gc-sections", "-pthread", "-ldl", "-lm", "-o", str(tmp / "probe")]
            subprocess.run(command, cwd=PLAYER, check=True)
            output = subprocess.check_output([str(tmp / "probe"), str(tmp / "fixture.db")], text=True)
            result.update(json.loads(output))
    if current:
        assert result["snapshot_blocked_ms"] < 100, result
        assert result["cache_marker_opens"] <= 800, result
        assert result["cache_free_space_checks"] <= 12, result
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
