#!/bin/sh
set -eu

mkdir -p third_party

fetch() {
    url="$1"
    out="$2"
    if [ ! -s "$out" ]; then
        echo "Downloading $out"
        curl -L --fail --silent --show-error "$url" -o "$out"
    fi
}

fetch "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h" \
      "third_party/dr_mp3.h"

fetch "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h" \
      "third_party/dr_wav.h"

fetch "https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c" \
      "third_party/stb_vorbis.c"

echo "Third-party audio decoders ready."
