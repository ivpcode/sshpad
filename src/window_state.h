#ifndef WINDOW_STATE_H
#define WINDOW_STATE_H

#include <gtk/gtk.h>

/* Legge ~/.config/sshpad/window.conf e applica dimensione alla finestra.
 * La posizione viene ripristinata dopo il mapping via X11. */
void window_state_load(GtkWindow *window);

/* Collega il segnale close-request per salvare geometria alla chiusura. */
void window_state_connect(GtkWindow *window);

#endif /* WINDOW_STATE_H */
