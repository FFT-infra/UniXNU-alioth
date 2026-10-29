#!/bin/sh
# Pack the GPU addon this board boots with. The client list is the part that matters: the
# wallpaper runs in PosterBoard's extensions (AegirPoster, KaleidoscopePoster), not in PosterBoard,
# and while those two were unlisted they abort-looped (fifteen and twelve exits a boot) because they
# could not get a Metal device. iconservicesagent and SpringBoard draw the rest.
#
#   scripts/make-gpu-addon-alioth.sh OUTPUT
#
# It ran from a shell command for u260, which is what the record cites; this file is that command.
set -e
OUT=${1:?usage: make-gpu-addon-alioth.sh OUTPUT}
HERE=$(cd "$(dirname "$0")" && pwd)
python3 "$HERE/../../UniXNU/scripts/make-gpu-addon.py" --publish \
    --clients AegirPoster,KaleidoscopePoster,SpringBoard,iconservicesagent \
    --selftest "$OUT"
