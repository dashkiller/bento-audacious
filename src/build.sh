#!/bin/sh
# Build bento.so — Audacious 4.x full interface plugin (Qt).
# Links against libaudqt (required for audqt::init / QtIfacePlugin).
set -e
cd "$(dirname "$0")"

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

g++ -std=c++17 -O2 -fPIC -shared -Wall bento.cc -o bento.so $CFLAGS $LIBS

# Verify the critical audqt symbols resolved
if nm -D bento.so 2>/dev/null | grep -q ' U .*prefswin_show'; then
    echo "WARNING: prefswin_show is still UNDEFINED — libaudqt did not link!"
    echo "         Try:  LIBS extra path, e.g.  LIBRARY_PATH=/usr/lib64 ./build.sh"
    ldd bento.so || true
    exit 1
fi

echo
echo "Built: $(pwd)/bento.so"
ldd bento.so | grep -E 'audqt|Qt' || true
echo
GENDIR=$(find /usr/lib /usr/lib64 /usr/local/lib -type d -path '*audacious/General' 2>/dev/null | head -1)
[ -z "$GENDIR" ] && GENDIR="/usr/lib/audacious/General"
echo "Install with:"
echo "  sudo cp bento.so \"$GENDIR/\""
echo
echo "Then fully quit Audacious and restart before switching interface."
