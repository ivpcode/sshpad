/*
 * terminal_launch.c — Rilevamento e avvio di un terminale nativo per SSHPad.
 *
 * Scansiona in ordine una lista di emulatori di terminale comuni; al primo
 * trovato tramite `which` esegue un fork/exec che apre una nuova finestra con
 * la sessione SSH già avviata. Il processo figlio viene staccato dal gruppo di
 * processo padre tramite setsid() così che la chiusura della UI non termini
 * la sessione SSH interattiva.
 *
 * Compilato con -std=c11 -Wall -Wextra -Wpedantic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "terminal_launch.h"

pid_t launch_terminal_with_ssh(const char *host_alias)
{
    if (!host_alias || !host_alias[0])
        return -1;

    /* Comando SSH che verrà eseguito all'interno del terminale.
     * La dimensione è generosa per ospitare alias arbitrariamente lunghi. */
    char ssh_cmd[256];
    snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s", host_alias);

    /* Tabella degli emulatori di terminale supportati.
     * - check: nome del binario cercato tramite `which`.
     * - fmt:   formato della riga di comando dove %s viene sostituito con
     *          ssh_cmd. Le varianti che accettano -e o -- sh -c mantengono
     *          la shell aperta dopo l'uscita di ssh tramite `exec bash`. */
    typedef struct {
        const char *check;
        const char *fmt;
    } term_t;

    static const term_t terminals[] = {
        { "gnome-terminal",      "gnome-terminal -- sh -c '%s; exec bash'"          },
        { "konsole",             "konsole -e sh -c '%s; exec bash'"                 },
        { "xfce4-terminal",      "xfce4-terminal -e 'sh -c \"%s; exec bash\"'"      },
        { "alacritty",           "alacritty -e sh -c '%s; exec bash'"               },
        { "kitty",               "kitty sh -c '%s; exec bash'"                      },
        { "foot",                "foot sh -c '%s; exec bash'"                       },
        { "x-terminal-emulator", "x-terminal-emulator -e 'sh -c \"%s\"'"            },
        { "xterm",               "xterm -e 'sh -c \"%s\"'"                          },
        { NULL, NULL },
    };

    for (const term_t *t = terminals; t->check != NULL; t++) {

        /* Verifica che il binario sia presente nel PATH. */
        char which_cmd[128];
        snprintf(which_cmd, sizeof(which_cmd),
                 "which %s >/dev/null 2>&1", t->check);

        if (system(which_cmd) != 0)
            continue;

        /* Il terminale è disponibile: lancia il figlio. */
        pid_t pid = fork();

        if (pid < 0) {
            /* fork fallita: ritorna errore al chiamante. */
            perror("fork");
            return -1;
        }

        if (pid == 0) {
            /* --- Processo figlio ---------------------------------------- */

            /* Crea una nuova sessione così il figlio non riceve i segnali
             * inviati al gruppo di processo della UI (es. SIGHUP). */
            setsid();

            /* Costruisce la riga di comando completa. */
            char full_cmd[1024];
            snprintf(full_cmd, sizeof(full_cmd), t->fmt, ssh_cmd);

            /* Esegue tramite la shell per consentire quoting/redirect. */
            execl("/bin/sh", "sh", "-c", full_cmd, NULL);

            /* execl ritorna solo in caso di errore. */
            perror("execl");
            _exit(127);
        }

        /* --- Processo padre --------------------------------------------- */
        /* Ritorna immediatamente il PID del figlio; non lo attende per non
         * bloccare il loop GTK. Il processo_manager del chiamante si occuperà
         * di waitpid. */
        return pid;
    }

    fprintf(stderr, "Nessun emulatore di terminale trovato\n");
    return -1;
}
