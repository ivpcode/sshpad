CC      = gcc
PKG     = gtk4 webkitgtk-6.0 libmicrohttpd json-c
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKG))
LDFLAGS = $(shell pkg-config --libs $(PKG)) -lpthread -lX11

SRC     = src/main.c \
          src/http_server.c \
          src/config_parser.c \
          src/process_manager.c \
          src/sse.c \
          src/terminal_launch.c \
          src/askpass.c \
          src/port_finder.c \
          src/util.c \
          src/window_state.c

TARGET  = sshpad

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
	install -Dm644 ui/index.html    $(DESTDIR)/usr/share/sshpad/ui/index.html
	install -Dm644 ui/style.css     $(DESTDIR)/usr/share/sshpad/ui/style.css
	install -Dm644 ui/app.js        $(DESTDIR)/usr/share/sshpad/ui/app.js
	install -Dm644 ui/terminal.svg  $(DESTDIR)/usr/share/sshpad/ui/terminal.svg

.PHONY: clean install
