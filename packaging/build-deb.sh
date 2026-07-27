#!/bin/sh
# packaging/build-deb.sh — builds hsed_<version>_amd64.deb from a clean
# checkout. Run from the packaging/ directory:
#
#   cd packaging && ./build-deb.sh
#
# Requires: gcc, make (for hsedd), go >= 1.24 (for hsed), dpkg-deb.
# Needs network access to fetch Go module dependencies the first time
# (only at build time — the resulting .deb is a static binary + a libc6-
# linked daemon, fully self-contained, no network needed to install or
# run). See tui/go.mod for a note on the golang.org/x/* replace
# directives this environment needed.
set -eu

VERSION=1.1.0
ARCH=amd64
PKG=hsed
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PKGROOT="$HERE/${PKG}_${VERSION}_${ARCH}"

echo "==> Building hsedd (C daemon)"
make -C "$ROOT/backend" clean
make -C "$ROOT/backend"

echo "==> Building hsed (Go TUI, static binary)"
( cd "$ROOT/tui" && CGO_ENABLED=0 go build -ldflags="-s -w" -o hsed . )

echo "==> Assembling package tree"
rm -rf "$PKGROOT"
mkdir -p "$PKGROOT/DEBIAN" \
         "$PKGROOT/usr/sbin" \
         "$PKGROOT/usr/bin" \
         "$PKGROOT/usr/share/doc/hsed" \
         "$PKGROOT/lib/systemd/system"

install -m755 "$ROOT/backend/hsedd" "$PKGROOT/usr/sbin/hsedd"
install -m755 "$ROOT/tui/hsed" "$PKGROOT/usr/bin/hsed"
install -m644 "$ROOT/backend/systemd/hsed.service" "$PKGROOT/lib/systemd/system/hsed.service"
install -m644 "$ROOT/README.md" "$PKGROOT/usr/share/doc/hsed/README.md"

# The small hand-written DEBIAN/{control,postinst,postrm} live in
# packaging/pkg-static/, never inside $PKGROOT — this directory gets
# rm -rf'd and rebuilt fresh every run, so anything meant to survive
# across builds has to live outside it.
STATIC="$HERE/pkg-static"
if [ ! -f "$STATIC/DEBIAN/control" ]; then
    echo "error: $STATIC/DEBIAN/control is missing — packaging/pkg-static/" >&2
    echo "       should be checked into version control; restore it before" >&2
    echo "       running this script." >&2
    exit 1
fi
cp "$STATIC/DEBIAN/control" "$PKGROOT/DEBIAN/control"
cp "$STATIC/DEBIAN/postinst" "$PKGROOT/DEBIAN/postinst"
cp "$STATIC/DEBIAN/postrm" "$PKGROOT/DEBIAN/postrm"
chmod 755 "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/postrm"
chmod 644 "$PKGROOT/DEBIAN/control"

SIZE_KB=$(du -sk --exclude=DEBIAN "$PKGROOT" | cut -f1)
if grep -q '^Installed-Size:' "$PKGROOT/DEBIAN/control"; then
    sed -i "s/^Installed-Size:.*/Installed-Size: $SIZE_KB/" "$PKGROOT/DEBIAN/control"
else
    sed -i "/^Priority:/a Installed-Size: $SIZE_KB" "$PKGROOT/DEBIAN/control"
fi

echo "==> Building .deb"
rm -f "$HERE/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKGROOT" "$HERE/${PKG}_${VERSION}_${ARCH}.deb"

echo "==> Done: $HERE/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --info "$HERE/${PKG}_${VERSION}_${ARCH}.deb"
