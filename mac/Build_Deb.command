#!/bin/bash
# Builds AmpEQ from source with Theos and puts the .deb in your Downloads folder.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/_common.sh"
echo "=== Building AmpEQ ==="
build_deb
DEB="$(ls -1 "$ROOT"/packages/*.deb | tail -n 1)"
cp "$DEB" "$HOME/Downloads/"
echo; echo "Done: $HOME/Downloads/$(basename "$DEB")"
echo "Install it with Sileo / Filza, or run Install_On_iPod.command."
