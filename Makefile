UNAME   := $(shell uname -s)
TARGET   = sshpad

# ---- Sorgenti condivise (backend POSIX) ----
COMMON_SRC = src/http_server.c \
             src/config_parser.c \
             src/process_manager.c \
             src/sse.c \
             src/terminal_launch.c \
             src/askpass.c \
             src/port_finder.c \
             src/util.c

# ---- Piattaforma ----
ifeq ($(UNAME),Darwin)
  # macOS (Apple Silicon / Intel)
  CC       = clang
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  PKG_CFG  = PKG_CONFIG_PATH=$(BREW_PREFIX)/lib/pkgconfig pkg-config
  PKG      = libmicrohttpd json-c
  CFLAGS   = -Wall -Wextra -O2 -ObjC $(shell $(PKG_CFG) --cflags $(PKG))
  LDFLAGS  = $(shell $(PKG_CFG) --libs $(PKG)) -lpthread \
             -framework Cocoa -framework WebKit
  SRC      = src/main_macos.m $(COMMON_SRC)
else
  # Linux
  CC       = gcc
  PKG      = gtk4 webkitgtk-6.0 libmicrohttpd json-c
  CFLAGS   = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKG))
  LDFLAGS  = $(shell pkg-config --libs $(PKG)) -lpthread -lX11
  SRC      = src/main.c src/window_state.c $(COMMON_SRC)
endif

# ---- Build ----
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
	rm -rf SSHPad.app

# ---- Linux install ----
install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -Dm644 ui/index.html    $(DESTDIR)/usr/share/sshpad/ui/index.html
	install -Dm644 ui/style.css     $(DESTDIR)/usr/share/sshpad/ui/style.css
	install -Dm644 ui/app.js        $(DESTDIR)/usr/share/sshpad/ui/app.js
	install -Dm644 ui/terminal.svg  $(DESTDIR)/usr/share/sshpad/ui/terminal.svg

# ---- macOS .app bundle ----
app: $(TARGET)
	@echo "Creating SSHPad.app bundle..."
	mkdir -p SSHPad.app/Contents/MacOS
	mkdir -p SSHPad.app/Contents/Resources/ui
	cp packaging/Info.plist SSHPad.app/Contents/
	cp $(TARGET) SSHPad.app/Contents/MacOS/
	cp ui/index.html ui/style.css ui/app.js ui/terminal.svg \
	   SSHPad.app/Contents/Resources/ui/
	@echo "SSHPad.app created."

.PHONY: clean install app
