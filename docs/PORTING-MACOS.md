# Porting SSHPad su macOS Apple Silicon

## Strategia

SSHPad usa GTK4 + WebKitGTK per la GUI su Linux. **WebKitGTK non esiste su macOS**, quindi il porting sostituisce solo il "guscio" GUI con un wrapper Objective-C minimale (Cocoa + WKWebView) che apre una `WKWebView` su `http://127.0.0.1:<porta>`.

Il backend C (HTTP server, process manager, SSE, config parser, askpass) è POSIX puro e funziona senza modifiche. L'UI HTML/JS/CSS è identica.

## Dipendenze macOS

```bash
xcode-select --install                    # clang, iconutil, hdiutil
brew install libmicrohttpd json-c pkg-config librsvg
```

Cocoa e WebKit.framework sono inclusi in macOS.

## Build

```bash
make clean && make          # auto-detect: compila con clang + frameworks
./sshpad                    # test diretto
make app                    # crea SSHPad.app bundle
open SSHPad.app             # lancia dal bundle
./packaging/build-dmg.sh    # crea .dmg per distribuzione
```

## File creati

| File | Descrizione |
|---|---|
| `src/main_macos.m` | Entry point Cocoa: NSApplication + NSWindow + WKWebView |
| `packaging/Info.plist` | Metadata .app bundle |
| `packaging/build-dmg.sh` | Script packaging: .app + .dmg |
| `docs/PORTING-MACOS.md` | Questo documento |

## File modificati

| File | Modifica |
|---|---|
| `src/http_server.c` | `#ifdef __APPLE__`: `_NSGetExecutablePath()` + path `.app/Contents/Resources/ui` |
| `src/terminal_launch.c` | `#ifdef __APPLE__`: Terminal.app, iTerm2 (AppleScript), Alacritty, Kitty |
| `Makefile` | Build condizionale Linux/Darwin con auto-detect `uname -s` |

## File invariati

Il backend POSIX puro non richiede modifiche:

- `src/process_manager.c` — fork/exec/waitpid/kill
- `src/sse.c` — pipe/fcntl
- `src/port_finder.c` — socket bind
- `src/config_parser.c` — file I/O
- `src/askpass.c` — mkstemp/pthread
- `src/util.c` — /dev/urandom (funziona su macOS)
- `src/main.c` — resta Linux-only
- `src/window_state.c` — resta Linux-only (X11)
- `ui/*` — HTML/CSS/JS identici

## Architettura macOS

```
NSApplication
  └─ AppDelegate
       ├─ applicationDidFinishLaunching:
       │    ├─ find_free_port()
       │    ├─ parse_ssh_config()
       │    ├─ sse_broadcaster_create()
       │    ├─ process_manager_create()
       │    ├─ http_server_start()
       │    ├─ NSWindow + WKWebView → http://127.0.0.1:<port>
       │    └─ setupMenuBar (Cmd+Q, Edit menu per Cmd+C/V/A)
       └─ applicationWillTerminate:
            ├─ MHD_stop_daemon()
            ├─ process_manager_kill_all()
            └─ cleanup risorse
```

## Struttura .app bundle

```
SSHPad.app/Contents/
├── Info.plist
├── MacOS/
│   └── sshpad
└── Resources/
    ├── sshpad.icns (generata da terminal.svg)
    └── ui/
        ├── index.html
        ├── style.css
        ├── app.js
        └── terminal.svg
```

## Note

1. **WKWebView e http://** — `127.0.0.1` è esente da App Transport Security, ma `NSAllowsLocalNetworking` è aggiunto in Info.plist per sicurezza
2. **Code signing** — App non firmata bloccata da Gatekeeper. Per uso personale: `xattr -cr SSHPad.app`. Per distribuzione: serve Developer ID + notarization
3. **Persistenza finestra** — `setFrameAutosaveName:` salva automaticamente posizione/dimensione (equivalente di `window_state.c` su Linux)
4. **`clock_gettime`** — disponibile da macOS 10.12, nessun problema con target minimo 12.0
