#ifndef PORT_FINDER_H
#define PORT_FINDER_H

/* Trova una porta libera su bind_addr (es. "127.0.0.1").
 * Ritorna il numero di porta, oppure -1 in caso di errore. */
int find_free_port(const char *bind_addr);

#endif /* PORT_FINDER_H */
