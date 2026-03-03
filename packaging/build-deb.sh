#!/bin/bash
set -euo pipefail

VERSION="1.0.0"
ARCH="amd64"
PKG_NAME="sshpad"
DEB_FILE="${PKG_NAME}_${VERSION}_${ARCH}.deb"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_DIR="$PROJECT_DIR/pkg"

echo "==> Building ${PKG_NAME}..."
cd "$PROJECT_DIR"
make clean
make

echo "==> Creating package structure..."
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/bin"
mkdir -p "$PKG_DIR/usr/share/sshpad/ui"
mkdir -p "$PKG_DIR/usr/share/applications"
mkdir -p "$PKG_DIR/usr/share/icons/hicolor/scalable/apps"

# Binary
install -m 755 "$PROJECT_DIR/sshpad" "$PKG_DIR/usr/bin/sshpad"

# UI files
install -m 644 "$PROJECT_DIR/ui/index.html"   "$PKG_DIR/usr/share/sshpad/ui/index.html"
install -m 644 "$PROJECT_DIR/ui/style.css"     "$PKG_DIR/usr/share/sshpad/ui/style.css"
install -m 644 "$PROJECT_DIR/ui/app.js"        "$PKG_DIR/usr/share/sshpad/ui/app.js"
install -m 644 "$PROJECT_DIR/ui/terminal.svg"  "$PKG_DIR/usr/share/sshpad/ui/terminal.svg"

# Desktop entry (nome deve corrispondere all'app ID GTK)
install -m 644 "$PROJECT_DIR/io.github.sshpad.desktop" "$PKG_DIR/usr/share/applications/io.github.sshpad.desktop"

# Icon (reuse terminal.svg)
install -m 644 "$PROJECT_DIR/ui/terminal.svg" "$PKG_DIR/usr/share/icons/hicolor/scalable/apps/sshpad.svg"

# Installed size (in KiB)
INSTALLED_SIZE=$(du -sk "$PKG_DIR" | cut -f1)

# DEBIAN/control
cat > "$PKG_DIR/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Architecture: ${ARCH}
Depends: libgtk-4-1, libwebkitgtk-6.0-4, libmicrohttpd12t64, libjson-c5, libx11-6, openssh-client
Section: net
Priority: optional
Installed-Size: ${INSTALLED_SIZE}
Maintainer: SSHPad <sshpad@localhost>
Description: SSH connection manager with GTK4/WebKitGTK UI
 SSHPad reads ~/.ssh/config and provides a graphical interface
 to manage SSH tunnels and open terminal sessions.
EOF

echo "==> Building .deb package..."
dpkg-deb --build "$PKG_DIR" "$PROJECT_DIR/$DEB_FILE"

echo "==> Cleaning up..."
rm -rf "$PKG_DIR"

echo "==> Done: $DEB_FILE"
echo ""
echo "Install:    sudo dpkg -i $DEB_FILE"
echo "Uninstall:  sudo dpkg -r $PKG_NAME"
