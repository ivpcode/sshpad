#!/bin/bash
# build-deb.sh — Crea il pacchetto .deb di SSHPad
set -e

VERSION=$(date +%Y.%m.%d)
ARCH=$(dpkg --print-architecture)
PKG="sshpad_${VERSION}_${ARCH}"
WORKDIR=$(mktemp -d)
STAGEDIR="$WORKDIR/stage"

cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "→ Preparazione struttura pacchetto $PKG..."

mkdir -p "$STAGEDIR/DEBIAN"
mkdir -p "$STAGEDIR/usr/bin"
mkdir -p "$STAGEDIR/usr/share/applications"
mkdir -p "$STAGEDIR/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$STAGEDIR/usr/share/sshpad/ui"

# Binario
if [ ! -f "sshpad" ]; then
    echo "ERRORE: binario 'sshpad' non trovato. Eseguire prima 'npm run build'." >&2
    exit 1
fi
cp sshpad "$STAGEDIR/usr/bin/sshpad"
chmod 755 "$STAGEDIR/usr/bin/sshpad"

# Desktop entry
cp io.github.sshpad.desktop "$STAGEDIR/usr/share/applications/"

# Icona (terminal.svg usata come icona app)
cp ui/terminal.svg "$STAGEDIR/usr/share/icons/hicolor/scalable/apps/sshpad.svg"

# UI (bundle Vite)
if [ ! -d "ui/dist" ]; then
    echo "ERRORE: ui/dist/ non trovato. Eseguire prima 'npm run ui:build'." >&2
    exit 1
fi
cp -r ui/dist/. "$STAGEDIR/usr/share/sshpad/ui/"

# Calcola dimensione installata (in KB)
INSTALLED_SIZE=$(du -sk "$STAGEDIR" | cut -f1)

# File di controllo
cat > "$STAGEDIR/DEBIAN/control" <<EOF
Package: sshpad
Version: $VERSION
Architecture: $ARCH
Depends: libgtk-4-1, libwebkitgtk-6.0-4, libmicrohttpd12t64, libjson-c5, libssl3, libcurl4, libx11-6, openssh-client
Section: net
Priority: optional
Installed-Size: $INSTALLED_SIZE
Maintainer: SSHPad <sshpad@localhost>
Description: SSH connection manager with GTK4/WebKitGTK UI
 SSHPad reads ~/.ssh/config and provides a graphical interface
 to manage SSH tunnels and open terminal sessions.
 Supports encrypted cloud sync via Cloudflare R2.
EOF

echo "→ Build pacchetto .deb..."
dpkg-deb --build --root-owner-group "$STAGEDIR" "${PKG}.deb"

echo "✓ Pacchetto creato: ${PKG}.deb"
