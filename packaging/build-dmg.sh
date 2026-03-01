#!/bin/bash
#
# build-dmg.sh — Crea SSHPad.app e .dmg per macOS
#
# Requisiti:
#   xcode-select --install
#   brew install libmicrohttpd json-c pkg-config librsvg
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_NAME="SSHPad"
APP_BUNDLE="${PROJECT_DIR}/${APP_NAME}.app"
DMG_NAME="${APP_NAME}.dmg"
VERSION="1.0.0"

echo "==> Building sshpad..."
cd "$PROJECT_DIR"
make clean
make

echo "==> Creating ${APP_NAME}.app bundle..."
make app

# ---- Icona (opzionale: richiede rsvg-convert + iconutil) ----
if command -v rsvg-convert &>/dev/null && [ -f "ui/terminal.svg" ]; then
    echo "==> Generating app icon from terminal.svg..."
    ICONSET_DIR=$(mktemp -d)/sshpad.iconset
    mkdir -p "$ICONSET_DIR"

    for SIZE in 16 32 64 128 256 512; do
        rsvg-convert -w $SIZE -h $SIZE ui/terminal.svg \
            -o "${ICONSET_DIR}/icon_${SIZE}x${SIZE}.png"
    done
    for SIZE in 16 32 128 256; do
        DOUBLE=$((SIZE * 2))
        cp "${ICONSET_DIR}/icon_${DOUBLE}x${DOUBLE}.png" \
           "${ICONSET_DIR}/icon_${SIZE}x${SIZE}@2x.png"
    done

    iconutil -c icns "$ICONSET_DIR" -o "${APP_BUNDLE}/Contents/Resources/sshpad.icns"
    rm -rf "$(dirname "$ICONSET_DIR")"
    echo "    Icon created."
else
    echo "    (Skipping icon: rsvg-convert not found or terminal.svg missing)"
fi

# ---- DMG ----
echo "==> Creating ${DMG_NAME}..."
DMG_STAGING=$(mktemp -d)
cp -R "$APP_BUNDLE" "$DMG_STAGING/"

# Symlink ad Applications per drag-and-drop install
ln -s /Applications "$DMG_STAGING/Applications"

hdiutil create -volname "$APP_NAME" \
    -srcfolder "$DMG_STAGING" \
    -ov -format UDZO \
    "${PROJECT_DIR}/${DMG_NAME}"

rm -rf "$DMG_STAGING"

echo ""
echo "Done! Created:"
echo "  ${APP_BUNDLE}"
echo "  ${PROJECT_DIR}/${DMG_NAME}"
echo ""
echo "Note: The app is not code-signed. To run it:"
echo "  xattr -cr ${APP_BUNDLE}"
echo "  open ${APP_BUNDLE}"
