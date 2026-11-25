#!/bin/sh
# Build bento.so — Audacious 4.x full interface plugin (Qt).
# Links against libaudqt (required for audqt::init / QtIfacePlugin).
#
# Layout:
#   ./build.sh          (this script, at project root)
#   ./src/bento.cc
#   ./src/bento_assets.h
#   ./bento.so          (output)
set -e
cd "$(dirname "$0")"

SRC=src
OUT=bento.so

if [ -z "$QT" ]; then
    LDCONFIG=$(command -v ldconfig || echo /sbin/ldconfig)
    AUDQT=$($LDCONFIG -p 2>/dev/null | awk '/libaudqt\.so /{print $NF; exit}')
    if [ -n "$AUDQT" ] && ldd "$AUDQT" | grep -q 'libQt6'; then
        QT=Qt6Widgets
    else
        QT=Qt5Widgets
    fi
fi
echo "Building against $QT + libaudqt"

CFLAGS=$(pkg-config --cflags audacious $QT)
LIBS=$(pkg-config --libs audacious $QT)

# Force-link libaudqt — required for audqt::init / QtIfacePlugin base class
if pkg-config --exists audqt 2>/dev/null; then
    CFLAGS="$CFLAGS $(pkg-config --cflags audqt)"
    LIBS="$LIBS $(pkg-config --libs audqt)"
elif pkg-config --exists libaudqt 2>/dev/null; then
    CFLAGS="$CFLAGS $(pkg-config --cflags libaudqt)"
    LIBS="$LIBS $(pkg-config --libs libaudqt)"
else
    LIBS="$LIBS -laudqt"
fi

echo "CFLAGS: $CFLAGS"
echo "LIBS:   $LIBS"

g++ -std=c++17 -O2 -fPIC -shared -Wall -I"$SRC" "$SRC/bento.cc" -o "$OUT" $CFLAGS $LIBS

# Verify the critical audqt symbols resolved
if nm -D "$OUT" 2>/dev/null | grep -q ' U .*prefswin_show'; then
    echo "WARNING: prefswin_show is still UNDEFINED — libaudqt did not link!"
    echo "         Try:  LIBS extra path, e.g.  LIBRARY_PATH=/usr/lib64 ./build.sh"
    ldd "$OUT" || true
fi

REAL_OUT=$(pwd)/bento.so
echo
echo "Built: $REAL_OUT"
ldd "$OUT" | grep -E 'audqt|Qt' || true
echo
GENDIR=$(find /usr/lib /usr/lib64 /usr/local/lib -type d -path '*audacious/General' 2>/dev/null | head -1)
[ -z "$GENDIR" ] && GENDIR="/usr/lib/audacious/General"
echo "Install with:"
echo "  sudo cp \"$REAL_OUT\" \"$GENDIR/\""
echo ""
echo "Then fully quit Audacious and restart before switching interface."
