#!/bin/sh
# ----------------------------------------------------------------------------------------------------
# p01-run.sh - starts p01-app (run as root: /dev/mem, /dev/dpu, framebuffer):
#   * libMali fbdev backend first in the library path (the rootfs default is the X11 backend);
#     the links live in /run/p01/lib (tmpfs)
#   * framebuffer console unbound, so the console does not draw over the video
# Usage: p01-run.sh [p01-app options]      (installed in /usr/bin; p01.service passes $P01_ARGS)
# ----------------------------------------------------------------------------------------------------
D=$(cd "$(dirname "$0")" && pwd)
L=/run/p01/lib
mkdir -p "$L"
for l in libMali.so.9 libEGL.so.1 libGLESv2.so.2; do
  ln -sf /usr/lib/fbdev/libMali.so.9.0 "$L/$l"
done
for v in /sys/class/vtconsole/vtcon*; do
  grep -q "frame buffer" "$v/name" 2>/dev/null && echo 0 > "$v/bind"
done
export LD_LIBRARY_PATH="$L${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$D/p01-app" "$@"
