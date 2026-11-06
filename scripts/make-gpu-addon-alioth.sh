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
WALLPAPER=${WALLPAPER:-}
# WALLPAPER names the overlay directory that scripts/make-wallpaper-overlay.py produces: the
# wallpaper's index.xml and assetManifest.caml plus the J120 PNG pair that replaces the sealed HEIC
# images, which ImageIO cannot decode on this board without a hardware HEVC decoder. The kernel
# mounts the files over the sealed animation, so without it the panel shows the default wallpaper's
# background and nothing else. Leave it unset to pack a plugin-only addon.
# --clients and --selftest are gone: the addon builder that carries this tree no longer parses
# either. The client allow-list went when the plugin stopped reading NativeAdrenoMetal.clients and
# answered every process, and the selftest marker was a local addition to the base repository that
# the build no longer accepts. Passing them makes the builder print its usage and exit.
set -- --publish
if [ -n "${PRIME_LIST:-}" ]; then
    set -- "$@" --prime-list "$PRIME_LIST"
fi
if [ -n "${PIPELINE_PACK:-}" ]; then
    set -- "$@" --pipeline-pack "$PIPELINE_PACK"
fi
if [ -n "${CACHE_EXPORT:-}" ]; then
    set -- "$@" --cache-export
fi
if [ -n "$WALLPAPER" ]; then
    set -- "$@" --wallpaper "$WALLPAPER"
fi
/opt/homebrew/bin/python3.13 "$HERE/../../UniXNU/scripts/make-gpu-addon.py" "$@" "$OUT"
