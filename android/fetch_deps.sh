#!/bin/bash
# Downloads the libraries the Android build compiles from source (into android/deps).
set -e
cd "$(dirname "$0")"
mkdir -p deps && cd deps
clone() { [ -d "$1" ] || git clone --depth 1 -b "$2" "$3" "$1"; }
clone SDL2 release-2.30.9 https://github.com/libsdl-org/SDL.git
clone libwebp v1.4.0 https://github.com/webmproject/libwebp.git
if [ ! -f sqlite/sqlite3.c ]; then
  mkdir -p sqlite
  curl -fsSL --retry 3 -o sqlite.zip https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  python3 -c "import zipfile,os;z=zipfile.ZipFile('sqlite.zip');[open('sqlite/'+os.path.basename(n),'wb').write(z.read(n)) for n in z.namelist() if not n.endswith('/')]"
fi
# unrar (for .cbr comics): optional, the build carries on without it
if [ ! -f unrar_srcs.txt ]; then
  if curl -fsSL --retry 3 -o unrar.tar.gz https://www.rarlab.com/rar/unrarsrc-6.2.12.tar.gz && tar xzf unrar.tar.gz; then
    (cd unrar && make -n lib 2>/dev/null | grep -o -- '-c [A-Za-z0-9_]*\.cpp' | sed 's/-c //' | sort -u) > unrar_srcs.txt
    mkdir -p unrar_inc
    printf '#ifndef _UNIX\n#define _UNIX\n#endif\n#include "../unrar/dll.hpp"\n' > unrar_inc/unrar.h
    echo "unrar: $(wc -l < unrar_srcs.txt) files"
  else
    echo "unrar not available: .cbr comics will be off"
  fi
fi
ls
