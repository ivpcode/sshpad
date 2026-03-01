#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

#include "window_state.h"

typedef struct {
    int width, height;
    int x, y;
    int has_pos;
} geom_t;

/* ---- Percorso file di configurazione ------------------------------------ */

static const char *config_path(void)
{
    static char path[512];
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg && xdg[0])
        snprintf(path, sizeof(path), "%s/sshpad/window.conf", xdg);
    else if (home && home[0])
        snprintf(path, sizeof(path), "%s/.config/sshpad/window.conf", home);
    else
        snprintf(path, sizeof(path), "/tmp/sshpad-window.conf");

    return path;
}

static void ensure_config_dir(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", config_path());
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
}

/* ---- Lettura / scrittura ------------------------------------------------ */

static geom_t load_geom(void)
{
    geom_t g = { 960, 680, 0, 0, 0 };
    FILE *f = fopen(config_path(), "r");
    if (!f) return g;

    int w = 0, h = 0, x = 0, y = 0;
    int n = fscanf(f, "width=%d height=%d x=%d y=%d", &w, &h, &x, &y);
    fclose(f);

    if (n >= 2 && w >= 200 && h >= 100) {
        g.width  = w;
        g.height = h;
    }
    if (n == 4) {
        g.x       = x;
        g.y       = y;
        g.has_pos = 1;
    }
    return g;
}

static void save_geom(GtkWindow *win)
{
    int w = gtk_widget_get_width(GTK_WIDGET(win));
    int h = gtk_widget_get_height(GTK_WIDGET(win));
    if (w < 100 || h < 100) return;

    int x = 0, y = 0;

#ifdef GDK_WINDOWING_X11
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win));
    if (surface && GDK_IS_X11_SURFACE(surface)) {
        Window      xwin     = gdk_x11_surface_get_xid(surface);
        GdkDisplay *gdkdisp  = gdk_surface_get_display(surface);
        Display    *xdisplay = gdk_x11_display_get_xdisplay(gdkdisp);
        Window      root     = DefaultRootWindow(xdisplay);
        Window      child;
        XTranslateCoordinates(xdisplay, xwin, root, 0, 0, &x, &y, &child);
    }
#endif

    ensure_config_dir();
    FILE *f = fopen(config_path(), "w");
    if (!f) return;
    fprintf(f, "width=%d height=%d x=%d y=%d\n", w, h, x, y);
    fclose(f);
}

/* ---- Segnali ------------------------------------------------------------ */

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    (void)data;
    save_geom(win);
    return FALSE; /* lascia proseguire la chiusura */
}

#ifdef GDK_WINDOWING_X11

typedef struct {
    GtkWindow *win;
    int x, y;
} move_data_t;

static gboolean do_move_x11(gpointer data)
{
    move_data_t *md      = (move_data_t *)data;
    GdkSurface  *surface = gtk_native_get_surface(GTK_NATIVE(md->win));

    if (surface && GDK_IS_X11_SURFACE(surface)) {
        Window      xwin     = gdk_x11_surface_get_xid(surface);
        GdkDisplay *gdkdisp  = gdk_surface_get_display(surface);
        Display    *xdisplay = gdk_x11_display_get_xdisplay(gdkdisp);
        XMoveWindow(xdisplay, xwin, md->x, md->y);
        XFlush(xdisplay);
    }
    free(md);
    return G_SOURCE_REMOVE;
}

static void on_map(GtkWidget *widget, gpointer data)
{
    /* Lascia al WM il tempo di processare il mapping, poi sposta */
    (void)widget;
    g_idle_add(do_move_x11, data);
    g_signal_handlers_disconnect_by_func(widget, on_map, data);
}

#endif /* GDK_WINDOWING_X11 */

/* ---- API pubblica ------------------------------------------------------- */

void window_state_load(GtkWindow *window)
{
    geom_t g = load_geom();
    gtk_window_set_default_size(window, g.width, g.height);

#ifdef GDK_WINDOWING_X11
    if (g.has_pos) {
        move_data_t *md = malloc(sizeof(move_data_t));
        if (md) {
            md->win = window;
            md->x   = g.x;
            md->y   = g.y;
            g_signal_connect(GTK_WIDGET(window), "map", G_CALLBACK(on_map), md);
        }
    }
#endif
}

void window_state_connect(GtkWindow *window)
{
    g_signal_connect(window, "close-request",
                     G_CALLBACK(on_close_request), NULL);
}
