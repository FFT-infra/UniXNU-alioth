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
# The prime list must name the iPadOS image's .metallib files, not the build host's. The tree's
# build/prime-list.txt is a macOS list (159 paths under /Applications and /System/Library), and
# most of those paths also exist on iPadOS, so the pass looked healthy while the libraries it
# never reached (QuartzCore's default.metallib among them) stayed uncompiled. configs/ holds the
# list read out of this board's donor image (21H461): 168 paths, of which 159 stat on the board.
PRIME_LIST=${PRIME_LIST:-"$HERE/../configs/prime-list-ios.txt"}
if [ -n "$PRIME_LIST" ]; then
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
# WALLPAPER_BUNDLE carries the owner @3x wallpaper bundle over the sealed collection. The device
# reports @3x and the sealed bundle declares @2x, so WallpaperKit synthesizes a black wallpaper;
# configs/wallpaper-bundle/ holds a bundle whose Wallpaper.plist declares @3x and whose .ca is
# filled by the PNG overlay.
WALLPAPER_BUNDLE=${WALLPAPER_BUNDLE:-"$HERE/../configs/wallpaper-bundle"}
set -- "$@" --wallpaper-bundle "$WALLPAPER_BUNDLE"
/opt/homebrew/bin/python3.13 "$HERE/../../UniXNU/scripts/make-gpu-addon.py" "$@" "$OUT"
