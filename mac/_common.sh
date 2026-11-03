# shared helpers for the AmpEQ Mac scripts (sourced, not run)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PORT="${AMPEQ_SSH_PORT:-2222}"
HOSTSPEC="mobile@127.0.0.1"
SSHOPTS=(-p "$PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=10)
RPATH='PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin; export PATH;'
rsh() { ssh "${SSHOPTS[@]}" "$HOSTSPEC" "$RPATH $*"; }
rsh_tty() { ssh -t "${SSHOPTS[@]}" "$HOSTSPEC" "$RPATH $*"; }
need_tunnel() {
  if ! ssh "${SSHOPTS[@]}" "$HOSTSPEC" 'true' 2>/dev/null; then
    echo "Can't reach the iPod on port $PORT."
    echo "Plug it in, unlock it, and run this in another Terminal window:  iproxy $PORT 22"
    echo "(OpenSSH must be installed on the iPod from Sileo.)"
    exit 3
  fi
}
build_deb() {
  export THEOS="${THEOS:-$HOME/theos}"
  [ -d "$THEOS" ] || { echo "Theos not found at $THEOS (install it from theos.dev)"; exit 1; }
  cd "$ROOT"
  rm -rf packages; make clean >/dev/null 2>&1 || true
  make package FINALPACKAGE=1 2>&1 | grep -vE "multiply_defined|CoreAudioTypes|UIUtilities"
  ls packages/*.deb >/dev/null 2>&1 || { echo "Build failed (see above)."; exit 1; }
}
