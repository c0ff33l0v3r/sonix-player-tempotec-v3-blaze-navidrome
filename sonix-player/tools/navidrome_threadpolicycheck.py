#!/usr/bin/env python3
"""Exercise actual production worker-entry policy helpers on one busy CPU.

Extracts only the entry policy call, not the worker body. Real artwork and GUI
flows are covered by their separate runners. No realtime privilege is needed.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HARNESS = r'''
#define _GNU_SOURCE
#include "src/system/core/utils.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static atomic_bool ready, go, done;
static atomic_int policy, nice_value;
static double seconds(void) {
 struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
 return t.tv_sec+t.tv_nsec/1e9;
}
static void *worker(void *unused) {
 (void)unused;
 POLICY("navidrome visible test");
 struct sched_param p; int kind;
 assert(!pthread_getschedparam(pthread_self(),&kind,&p));
 policy=kind; nice_value=getpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid));
 ready=true;
 while(!go) sched_yield();
 volatile uint32_t work=1;
 for(unsigned i=0;i<8000000;i++)work=work*1664525u+1013904223u;
 done=work!=0;
 return NULL;
}
int main(void) {
 cpu_set_t allowed,one; CPU_ZERO(&one);
 if(sched_getaffinity(0,sizeof(allowed),&allowed))return 77;
 int cpu=-1;
 for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&allowed)){cpu=i;break;}
 if(cpu<0)return 77;
 CPU_SET(cpu,&one);
 if(sched_setaffinity(0,sizeof(one),&one))return 77;
 pthread_t thread; assert(!pthread_create(&thread,NULL,worker,NULL));
 double until=seconds()+5;
 while(!ready && seconds()<until)sched_yield();
 assert(ready);
 if(policy!=SCHED_OTHER || nice_value!=10){go=true;pthread_join(thread,NULL);puts("entry policy starves visible class");return 2;}
 go=true;
 volatile uint32_t competing=1;
 while(!done && seconds()<until)competing=competing*1664525u+1013904223u;
 assert(done && "visible work must finish while competing CPU work remains active");
 pthread_join(thread,NULL);
 printf("actual nice-10 policy, one permitted CPU, progress under contention: passed (%u)\n",(unsigned)competing);
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    player = Path(__file__).resolve().parents[1]
    os.chdir(player)
    with tempfile.TemporaryDirectory(prefix='sonix-threadpolicy-') as tmp:
        path = Path(tmp)
        source = path / 'check.c'
        source.write_text(HARNESS)
        for filename, entry in [('navidromeart.c', 'art_worker'), ('navidromeart.c', 'art_network_worker'), ('navidromebrowse.c', 'read_windows')]:
            name = 'sonix-player/src/gui/streaming/' + filename
            text = subprocess.check_output(['git', 'show', 'a19cd44:' + name], text=True) if args.baseline else Path('src/gui/streaming', filename).read_text()
            match = re.search(r'static void \*' + entry + r'\(void \*unused\) \{\s*\(void\)unused;\s*(thread_be_\w+)\(', text)
            assert match, 'Worker entry policy must be explicitly checked: ' + entry
            command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-I.', '-DPOLICY=' + match[1], str(source), 'src/system/core/utils.c', '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(path/'check')]
            subprocess.run(command, check=True)
            result = subprocess.run([str(path/'check')], capture_output=True, text=True, timeout=12)
            if result.returncode == 77:
                raise SystemExit('Unsupported: effective CPU affinity could not be constrained; no pass claimed.')
            if args.baseline:
                assert result.returncode == 2 and 'entry policy' in result.stdout, result.stderr
                print(entry + ': baseline idle-class policy reproduced')
            else:
                assert result.returncode == 0, result.stderr + result.stdout
                print(entry + ': ' + result.stdout.strip())

if __name__ == '__main__':
    main()
