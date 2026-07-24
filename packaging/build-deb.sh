#!/bin/sh
set -eu

VERSION=1.0.0
ARCH=amd64
PKG=hsed
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PKGROOT="$HERE/${PKG}_${VERSION}_${ARCH}"

echo "==> Building hsedd (C daemon)"
make -C "$ROOT/backend" clean
make -C "$ROOT/backend"

echo "==> Vendoring Python dependencies (textual + transitive deps)"
VENDOR_BUILD="$HERE/vendor-build"
rm -rf "$VENDOR_BUILD"
mkdir -p "$VENDOR_BUILD"
pip install --break-system-packages --target="$VENDOR_BUILD" "textual>=0.60.0"
find "$VENDOR_BUILD" -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
find "$VENDOR_BUILD" -iname "*.pyc" -delete
rm -rf "$VENDOR_BUILD/bin"   
find "$VENDOR_BUILD" -iname "*.dist-info" -type d | while read -r d; do
    find "$d" -type f ! -name METADATA ! -name RECORD ! -name INSTALLER -delete 2>/dev/null || true
done

echo "==> Assembling package tree"
rm -rf "$PKGROOT"
mkdir -p "$PKGROOT/DEBIAN" \
         "$PKGROOT/usr/sbin" \
         "$PKGROOT/usr/bin" \
         "$PKGROOT/usr/share/hsed" \
         "$PKGROOT/usr/share/doc/hsed" \
         "$PKGROOT/lib/systemd/system"

install -m755 "$ROOT/backend/hsedd" "$PKGROOT/usr/sbin/hsedd"
install -m644 "$ROOT/backend/systemd/hsed.service" "$PKGROOT/lib/systemd/system/hsed.service"
install -m644 "$ROOT/README.md" "$PKGROOT/usr/share/doc/hsed/README.md"

cp -r "$VENDOR_BUILD" "$PKGROOT/usr/share/hsed/vendor"
cp -r "$ROOT/hidden_space_explorer" "$PKGROOT/usr/share/hsed/hidden_space_explorer"
find "$PKGROOT/usr/share/hsed" -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
find "$PKGROOT/usr/share/hsed" -iname "*.pyc" -delete

STATIC="$HERE/pkg-static"
if [ ! -f "$STATIC/DEBIAN/control" ] || [ ! -f "$STATIC/usr/bin/hsed" ]; then
    echo "error: $STATIC is missing control/hsed — packaging/pkg-static/" >&2
    echo "       should be checked into version control; restore it before" >&2
    echo "       running this script." >&2
    exit 1
fi
cp "$STATIC/DEBIAN/control" "$PKGROOT/DEBIAN/control"
cp "$STATIC/DEBIAN/postinst" "$PKGROOT/DEBIAN/postinst"
cp "$STATIC/DEBIAN/postrm" "$PKGROOT/DEBIAN/postrm"
cp "$STATIC/usr/bin/hsed" "$PKGROOT/usr/bin/hsed"
chmod 755 "$PKGROOT/usr/bin/hsed"
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
