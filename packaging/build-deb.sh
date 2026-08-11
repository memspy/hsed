#!/bin/sh
set -eu

VERSION=1.1.1
PKG=hsed
ARCH="${1:-amd64}"

case "$ARCH" in
    amd64) CC=gcc; GOARCH=amd64 ;;
    arm64) CC=aarch64-linux-gnu-gcc; GOARCH=arm64 ;;
    *)
        echo "usage: $0 [amd64|arm64]" >&2
        exit 2
        ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PKGROOT="$HERE/${PKG}_${VERSION}_${ARCH}"

echo "==> Building hsedd (C daemon, $ARCH via $CC)"
make -C "$ROOT/backend" clean
make -C "$ROOT/backend" CC="$CC"

echo "==> Building hsed (Go TUI, $ARCH, static binary)"
( cd "$ROOT/tui" && GOOS=linux GOARCH="$GOARCH" CGO_ENABLED=0 go build -ldflags="-s -w" -o hsed . )

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


sed -i "s/^Architecture:.*/Architecture: $ARCH/" "$PKGROOT/DEBIAN/control"

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
