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
# replaces older AmpEQ builds (com.ampeq.ipod7.test / com.kerbal.ampeq) if present; settings are kept
rsh_tty 'for old in com.ampeq.ipod7.test com.kerbal.ampeq; do dpkg -s $old >/dev/null 2>&1 && sudo dpkg -r $old; done; sudo dpkg -i /var/mobile/ampeq.deb; rm -f /var/mobile/ampeq.deb' || exit 1
echo "Respringing..."
rsh_tty 'sudo sbreload 2>/dev/null || sudo killall -9 SpringBoard'
echo; echo "Done. Open Settings > AmpEQ."
