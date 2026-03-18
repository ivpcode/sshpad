#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http_server.h"
#include "app_context.h"
#include "port_finder.h"
#include "config_parser.h"
#include "process_manager.h"
#include "sse.h"
#include "askpass.h"
#include "local_proxy.h"
#include "window_state.h"

static void on_activate(GtkApplication *gtkapp, gpointer user_data) {
    app_context_t *ctx = (app_context_t *)user_data;

    GtkWidget *window = gtk_application_window_new(gtkapp);
    gtk_window_set_title(GTK_WINDOW(window), "SSHPad");
    gtk_window_set_icon_name(GTK_WINDOW(window), "sshpad");
    window_state_load(GTK_WINDOW(window));
    window_state_connect(GTK_WINDOW(window));

    /* Disabilita cache WebKit per servire sempre i file aggiornati */
    WebKitWebContext *web_context = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(web_context,
                                       WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    WebKitWebView *webview = WEBKIT_WEB_VIEW(webkit_web_view_new());

    WebKitSettings *settings = webkit_web_view_get_settings(webview);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    char uri[64];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%d", ctx->port);
    webkit_web_view_load_uri(webview, uri);

    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(webview));
    gtk_window_present(GTK_WINDOW(window));
}

static void cleanup(app_context_t *ctx) {
    if (ctx->proxy) {
        lp_stop(ctx->proxy);
        lp_free(ctx->proxy);
    }
    if (ctx->httpd) MHD_stop_daemon(ctx->httpd);
    process_manager_kill_all(ctx->pm);
    process_manager_free(ctx->pm);
    sse_broadcaster_free(ctx->sse);
    ssh_hosts_free(ctx->hosts, ctx->num_hosts);
    askpass_cleanup(ctx->askpass_path);
}

/*
 * sanitize_snap_env — rimuove la contaminazione Snap dall'ambiente.
 *
 * Quando SSHPad viene lanciato dal terminale di VS Code (installato via
 * Snap), l'ambiente eredita variabili che puntano a /snap/code/... e
 * /snap/core20/... . I sotto-processi di WebKitGTK (WebKitNetworkProcess,
 * WebKitWebProcess) usano queste variabili per cercare moduli GIO, schemi
 * GSettings e librerie GTK, finendo per caricare la libpthread di core20
 * che è incompatibile con la glibc di sistema. Risultato:
 *   "symbol lookup error: __libc_pthread_init, version GLIBC_PRIVATE"
 *
 * La soluzione è ripristinare le variabili contaminate ai valori originali
 * (che VS Code Snap salva con suffisso _VSCODE_SNAP_ORIG) oppure rimuoverle
 * se erano vuote prima dello Snap.
 */
static void sanitize_snap_env(void)
{
    /* Verifica se siamo in un ambiente Snap (VS Code o altro) */
    const char *gtk_exe = getenv("GTK_EXE_PREFIX");
    if (!gtk_exe || !strstr(gtk_exe, "/snap/")) {
        /* Controllo secondario su LD_LIBRARY_PATH */
        const char *ldpath = getenv("LD_LIBRARY_PATH");
        if (!ldpath || !strstr(ldpath, "/snap/"))
            return;
    }

    /*
     * Variabili che VS Code Snap sovrascrive.
     * Per ciascuna, cerchiamo il valore originale salvato in *_VSCODE_SNAP_ORIG.
     * Se l'originale era vuoto, rimuoviamo la variabile.
     */
    static const char *snap_vars[] = {
        "GTK_PATH",
        "GTK_EXE_PREFIX",
        "GTK_IM_MODULE_FILE",
        "GIO_MODULE_DIR",
        "GSETTINGS_SCHEMA_DIR",
        "LOCPATH",
        "XDG_DATA_HOME",
        "XDG_DATA_DIRS",
        "XDG_CONFIG_DIRS",
        "GDK_BACKEND",
        "LD_LIBRARY_PATH",
        NULL
    };

    for (const char **var = snap_vars; *var; var++) {
        /* Cerca il valore originale salvato da VS Code Snap */
        char orig_key[128];
        snprintf(orig_key, sizeof(orig_key), "%s_VSCODE_SNAP_ORIG", *var);

        const char *orig_val = getenv(orig_key);
        if (orig_val) {
            /* VS Code Snap ha salvato il valore originale */
            if (orig_val[0] == '\0')
                unsetenv(*var);
            else
                setenv(*var, orig_val, 1);
        } else {
            /* Nessun backup: se il valore attuale punta a /snap/, rimuovilo */
            const char *cur = getenv(*var);
            if (cur && strstr(cur, "/snap/"))
                unsetenv(*var);
        }
    }
}

int main(int argc, char *argv[]) {
    /* Pulisce LD_LIBRARY_PATH dai path Snap che fanno crashare WebKit */
    sanitize_snap_env();

    /* WebKitGTK 6.0 usa bubblewrap per il sandboxing, che richiede
       unprivileged user namespaces nel kernel. Su molti sistemi questi
       sono disabilitati (es. Ubuntu 24.04+). Dato che carichiamo solo
       contenuti da 127.0.0.1, il sandbox non serve. */
    setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 0);

    app_context_t ctx = {0};

    ctx.port = find_free_port("127.0.0.1");
    if (ctx.port < 0) {
        fprintf(stderr, "Impossibile trovare una porta libera\n");
        return 1;
    }
    printf("Porta selezionata: %d\n", ctx.port);

    ctx.hosts = parse_ssh_config(NULL, &ctx.num_hosts);

    if (askpass_init(ctx.askpass_path, ctx.port) != 0) {
        fprintf(stderr, "Impossibile inizializzare askpass\n");
        return 1;
    }

    ctx.sse = sse_broadcaster_create();
    ctx.pm = process_manager_create(ctx.sse, ctx.askpass_path);

    ctx.httpd = http_server_start(ctx.port, &ctx);
    if (!ctx.httpd) {
        fprintf(stderr, "Impossibile avviare HTTP server\n");
        return 1;
    }

    /* HTTPS reverse proxy (opzionale: richiede mkcert) */
    if (lp_check_mkcert() == 0) {
        ctx.proxy = lp_create(ctx.hosts, ctx.num_hosts, ctx.sse);
        if (ctx.proxy) {
            if (lp_start(ctx.proxy) != 0)
                fprintf(stderr, "HTTPS proxy non avviato\n");
        }
    } else {
        fprintf(stderr,
                "mkcert non trovato. HTTPS proxy disabilitato.\n"
                "Per abilitarlo installa mkcert:\n"
                "  sudo apt install mkcert   (Debian/Ubuntu)\n"
                "  brew install mkcert       (macOS)\n"
                "Poi esegui: mkcert -install\n");
    }

    GtkApplication *gtkapp = gtk_application_new("io.github.sshpad",
                                                  G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &ctx);

    int status = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    g_object_unref(gtkapp);
    cleanup(&ctx);

    return status;
}
