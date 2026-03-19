#!/bin/bash
# release.sh — Crea una GitHub Release con il .deb del giorno
set -e

DATE=$(date +%Y.%m.%d)
VERSION="v${DATE}"
ARCH=$(dpkg --print-architecture)
DEB="sshpad_${DATE}_${ARCH}.deb"
NOTES=$(git log -1 --pretty=%B)

if [ ! -f "$DEB" ]; then
    echo "ERRORE: $DEB non trovato. Eseguire prima 'npm run dpkg:build'." >&2
    exit 1
fi

echo "→ Creazione release $VERSION..."
gh release create "$VERSION" "./$DEB" \
    --title "Release $VERSION" \
    --notes "$NOTES"

echo "✓ Release $VERSION pubblicata."
