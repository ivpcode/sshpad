#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <stdio.h>
#include <stdlib.h>
#include "http_server.h"
#include "app_context.h"
#include "port_finder.h"
#include "config_parser.h"
#include "process_manager.h"
#include "sse.h"
#include "window_state.h"

static void on_activate(GtkApplication *gtkapp, gpointer user_data) {
    app_context_t *ctx = (app_context_t *)user_data;

    GtkWidget *window = gtk_application_window_new(gtkapp);
    gtk_window_set_title(GTK_WINDOW(window), "SSHPad");
    gtk_window_set_icon_name(GTK_WINDOW(window), "sshpad");
    window_state_load(GTK_WINDOW(window));
    window_state_connect(GTK_WINDOW(window));

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
    if (ctx->httpd) MHD_stop_daemon(ctx->httpd);
    process_manager_kill_all(ctx->pm);
    process_manager_free(ctx->pm);
    sse_broadcaster_free(ctx->sse);
    ssh_hosts_free(ctx->hosts, ctx->num_hosts);
}

int main(int argc, char *argv[]) {
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

    ctx.sse = sse_broadcaster_create();
    ctx.pm = process_manager_create(ctx.sse);

    ctx.httpd = http_server_start(ctx.port, &ctx);
    if (!ctx.httpd) {
        fprintf(stderr, "Impossibile avviare HTTP server\n");
        return 1;
    }

    GtkApplication *gtkapp = gtk_application_new("io.github.sshpad",
                                                  G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &ctx);

    int status = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    g_object_unref(gtkapp);
    cleanup(&ctx);

    return status;
}
