"""Materialize and instrument committed pre-optimization sources in a temp dir."""
from pathlib import Path
import subprocess
import sys

REVISION = 'dc748027b0d3e4d665351ded6c6a1b5d8e581a0c'
root = Path(sys.argv[1])
git = ['git', '-C', str(Path(__file__).resolve().parents[2])]
paths = subprocess.check_output([*git, 'ls-tree', '-r', '--name-only', REVISION,
                                 'sonix-player/src/system/streaming'], text=True).splitlines()
paths = [p for p in paths if Path(p).name.startswith('navidrome')]
paths += ['sonix-player/src/system/' + name for name in
          ['core/json.c', 'core/json.h', 'net/http.c', 'net/http.h', 'net/tls.c', 'net/tls.h']]
for name in paths:
    dest = root / name.removeprefix('sonix-player/')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(subprocess.check_output([*git, 'show', f'{REVISION}:{name}']))
p = root / 'src/system/streaming/navidrome.c'
s = p.read_text().replace('#include <unistd.h>', '#include <unistd.h>\n#include <time.h>')
s = s.replace('static void sync_run(', '''static double baseline_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9;
}
static void sync_run(''')
s = s.replace('char before[128] = "", after[128] = "";', 'char before[128] = "", after[128] = "";\n    double request_ms = 0, import_ms = 0, validation_ms = 0;')
s = s.replace('\t\t\t\terror = nd_api_json(', '\t\t\t\tdouble request_start = baseline_now();\n\t\t\t\terror = nd_api_json(')
s = s.replace('\n\t\t\t\tif (error)\n\t\t\t\t\tnd_log("navidrome: sync request', '\n                request_ms += (baseline_now() - request_start) * 1000;\n\t\t\t\tif (error)\n\t\t\t\t\tnd_log("navidrome: sync request')
s = s.replace('\t\t\t\t\tif (!nd_store_import(', '\t\t\t\t\tdouble import_start = baseline_now();\n\t\t\t\t\tif (!nd_store_import(')
s = s.replace('error = store.error ? store.error : "nd_response_error";', 'error = store.error ? store.error : "nd_response_error";\n                    import_ms += (baseline_now() - import_start) * 1000;')
s = s.replace('bool leased = navidrome_cache_begin(', 'double validation_start = baseline_now();\n    bool leased = navidrome_cache_begin(')
s = s.replace('bool valid = sync_current(job)', 'validation_ms = (baseline_now() - validation_start) * 1000;\n    bool valid = sync_current(job)')
s = s.replace('bool published = !error && nd_store_publish(&store);', 'double publish_start = baseline_now();\n    bool published = !error && nd_store_publish(&store);\n    double publish_ms = (baseline_now() - publish_start) * 1000;')
s = s.replace('if (store.db)\n\t\tnd_store_cleanup(&store);', 'double cleanup_start = baseline_now();\n    if (store.db)\n\t\tnd_store_cleanup(&store);')
s = s.replace('nd_store_close(&store);\n\tif (leased)', 'nd_store_close(&store);\n    double cleanup_ms = (baseline_now() - cleanup_start) * 1000;\n\tif (leased)')
s = s.replace('nd_log("navidrome: sync finish', 'nd_log("navidrome: baseline timing request_ms=%.3f import_ms=%.3f validation_ms=%.3f publication_ms=%.3f cleanup_ms=%.3f\\n", request_ms, import_ms, validation_ms, publish_ms, cleanup_ms);\n    nd_log("navidrome: sync finish')
p.write_text(s)
