#!/usr/bin/env python3
"""Verify Music's existing prepared cache after shared pixel-I/O extraction.

Uses private production cache functions with an isolated directory. Does not
open the Music library or claim local-file extraction/player acceptance.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

CHECK = r'''
#include "src/gui/nowplaying/cover.c"
#include <assert.h>
static cover_image_t picture(int w, int h, unsigned salt) {
 FILE *file=tmpfile(); assert(file);
 for(int i=0;i<w*h;i++){unsigned char b[2]={(unsigned char)(i+salt),(unsigned char)(i*3+salt)};assert(fwrite(b,1,2,file)==2);}
 rewind(file);cover_image_t image={0};assert(cover_read_pixels(file,w,h,&image));fclose(file);return image;
}
int main(int argc,char **argv) {
 assert(argc==2);snprintf(playerart_path,sizeof(playerart_path),"%s",argv[1]);playerart_ready=true;
 for(int w=320;w<=480;w+=160)for(int studio=0;studio<2;studio++) {
  cover_set_backdrop_light(studio);cover_set_backdrop_upright(studio);
  unsigned policy=cover_render_policy();assert((policy&3)==(studio?3u:0u));
  char key[96];assert(playerart_key(key,sizeof(key),7,w,w,64,studio?64:32));
  cover_image_t cover=picture(w,w,3),backdrop=picture(64,studio?64:32,8),a={0},b={0};
  playerart_store(key,&cover,&backdrop);assert(playerart_load(key,&a,&b));
  assert(a.pixels!=cover.pixels && b.pixels!=backdrop.pixels);
  assert(a.dsc.data_size==cover.dsc.data_size && !memcmp(a.pixels,cover.pixels,a.dsc.data_size));
  assert(b.dsc.data_size==backdrop.dsc.data_size && !memcmp(b.pixels,backdrop.pixels,b.dsc.data_size));
  cover_free(&a);cover_free(&b);
  char path[600];assert(playerart_file(key,path,sizeof(path)));
  FILE *file=fopen(path,"r+b");assert(file);playerart_header_t header;
  assert(fread(&header,sizeof(header),1,file)==1);header.cw=513;rewind(file);assert(fwrite(&header,sizeof(header),1,file)==1);fclose(file);
  assert(!playerart_load(key,&a,&b) && !a.pixels && !b.pixels);
  assert(access(path,F_OK));cover_free(&cover);cover_free(&backdrop);
 }
 puts("Music prepared cover/backdrop cache: exact RGB565 round trips, independent ownership, render-policy keys and invalid dimensions passed");
}
'''

def main():
    player = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sonix-music-cache-') as directory:
        root = Path(directory)
        (root/'check.c').write_text(CHECK)
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections', '-DHOST_BUILD', '-DLV_CONF_INCLUDE_SIMPLE', '-I.', '-Ilvgl']
        command += shlex.split(os.environ.get('NAVIDROME_CHECK_FLAGS', ''))
        command += shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'freetype2', 'sdl2'], text=True))
        command += [str(root/'check.c'), '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(root/'check')]
        subprocess.run(command, cwd=player, check=True)
        subprocess.run([str(root/'check'), str(root)], cwd=player, check=True)

if __name__ == '__main__':
    main()
