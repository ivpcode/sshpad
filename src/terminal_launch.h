#ifndef TERMINAL_LAUNCH_H
#define TERMINAL_LAUNCH_H

#include <sys/types.h>

/* Detecta il terminale disponibile e lancia ssh host_alias in una nuova finestra.
 * Ritorna il PID del processo figlio oppure -1 in caso di errore. */
pid_t launch_terminal_with_ssh(const char *host_alias);

#endif /* TERMINAL_LAUNCH_H */
