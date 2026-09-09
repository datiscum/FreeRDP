#!/bin/sh
set -eu
NO_SHM_DISPLAY=$DISPLAY
export NO_SHM_DISPLAY
exec "$1" -f "$XAUTHORITY" -a -s '-screen 0 2560x1440x24 -nolisten tcp' "$2"
