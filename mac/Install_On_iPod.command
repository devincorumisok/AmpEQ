#!/bin/bash
# Installs AmpEQ on the iPod over USB. Uses the ready-made .deb in packages/;
# run with --build (or if there is no .deb) to build from source first.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/_common.sh"
if [ "${1:-}" = "--build" ] || ! ls "$ROOT"/packages/*.deb >/dev/null 2>&1; then
  echo "=== Building AmpEQ ==="; build_deb
fi
PKG="$(ls -1 "$ROOT"/packages/*.deb | tail -n 1)"
echo "=== Installing $(basename "$PKG") ==="
need_tunnel
scp -O -P "$PORT" -o StrictHostKeyChecking=accept-new "$PKG" "$HOSTSPEC:/var/mobile/ampeq.deb" || exit 1
echo "Enter the iPod's mobile password if asked:"
# replaces the old test build (com.ampeq.ipod7.test) if it is there; settings are kept
rsh_tty 'if dpkg -s com.ampeq.ipod7.test >/dev/null 2>&1; then sudo dpkg -r com.ampeq.ipod7.test; fi; sudo dpkg -i /var/mobile/ampeq.deb; rm -f /var/mobile/ampeq.deb' || exit 1
echo "Respringing..."
rsh_tty 'sudo sbreload 2>/dev/null || sudo killall -9 SpringBoard'
echo; echo "Done. Open Settings > AmpEQ."
