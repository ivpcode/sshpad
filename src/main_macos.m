/*
 * main_macos.m — Entry point macOS per SSHPad.
 *
 * Wrapper minimale Cocoa + WKWebView che sostituisce GTK+WebKitGTK.
 * Apre una finestra con WKWebView su http://127.0.0.1:<porta>, utilizzando
 * lo stesso backend C (HTTP server, process manager, SSE, config parser).
 *
 * Compilato con: clang -ObjC -framework Cocoa -framework WebKit
 */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <stdio.h>
#include <stdlib.h>

#include "app_context.h"
#include "http_server.h"
#include "port_finder.h"
#include "config_parser.h"
#include "process_manager.h"
#include "sse.h"

/* ------------------------------------------------------------------ */
/* Application Delegate                                                */
/* ------------------------------------------------------------------ */

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic) app_context_t ctx;
@property (strong, nonatomic) NSWindow *window;
@property (strong, nonatomic) WKWebView *webView;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;

    /* --- Backend C identico a main.c -------------------------------- */

    _ctx = (app_context_t){0};

    _ctx.port = find_free_port("127.0.0.1");
    if (_ctx.port < 0) {
        fprintf(stderr, "Impossibile trovare una porta libera\n");
        [NSApp terminate:nil];
        return;
    }
    printf("Porta selezionata: %d\n", _ctx.port);

    _ctx.hosts = parse_ssh_config(NULL, &_ctx.num_hosts);

    _ctx.sse = sse_broadcaster_create();
    _ctx.pm  = process_manager_create(_ctx.sse);

    _ctx.httpd = http_server_start(_ctx.port, &_ctx);
    if (!_ctx.httpd) {
        fprintf(stderr, "Impossibile avviare HTTP server\n");
        [NSApp terminate:nil];
        return;
    }

    /* --- Finestra --------------------------------------------------- */

    NSRect frame = NSMakeRect(0, 0, 1100, 700);
    NSWindowStyleMask style = NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable
                            | NSWindowStyleMaskMiniaturizable
                            | NSWindowStyleMaskResizable;

    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setTitle:@"SSHPad"];
    [_window setMinSize:NSMakeSize(600, 400)];
    [_window center];

    /* Salva/ripristina automaticamente posizione e dimensione finestra */
    [_window setFrameAutosaveName:@"SSHPadMainWindow"];

    /* --- WKWebView -------------------------------------------------- */

    WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];

    /* Abilita developer extras (Web Inspector con Cmd+Opt+I) */
    [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

    _webView = [[WKWebView alloc] initWithFrame:frame configuration:config];
    [_webView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    char uri[64];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%d", _ctx.port);
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:uri]];
    [_webView loadRequest:[NSURLRequest requestWithURL:url]];

    [_window setContentView:_webView];
    [_window makeKeyAndOrderFront:nil];

    /* --- Menu bar --------------------------------------------------- */

    [self setupMenuBar];
}

- (void)setupMenuBar
{
    NSMenu *mainMenu = [[NSMenu alloc] init];

    /* Menu applicazione (SSHPad) */
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"SSHPad"];
    [appMenu addItemWithTitle:@"About SSHPad"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit SSHPad"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];
    [mainMenu addItem:appMenuItem];

    /* Menu Edit (necessario per Cmd+C/V/X/A in WKWebView) */
    NSMenuItem *editMenuItem = [[NSMenuItem alloc] init];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Undo"   action:@selector(undo:)       keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"Redo"   action:@selector(redo:)       keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut"    action:@selector(cut:)        keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy"   action:@selector(copy:)       keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste"  action:@selector(paste:)      keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editMenuItem setSubmenu:editMenu];
    [mainMenu addItem:editMenuItem];

    /* Menu Window */
    NSMenuItem *windowMenuItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"     action:@selector(performZoom:)        keyEquivalent:@""];
    [windowMenuItem setSubmenu:windowMenu];
    [mainMenu addItem:windowMenuItem];

    [NSApp setMainMenu:mainMenu];
    [NSApp setWindowsMenu:windowMenu];
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
    (void)notification;

    if (_ctx.httpd)
        MHD_stop_daemon(_ctx.httpd);

    process_manager_kill_all(_ctx.pm);
    process_manager_free(_ctx.pm);
    sse_broadcaster_free(_ctx.sse);
    ssh_hosts_free(_ctx.hosts, _ctx.num_hosts);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];

        [app run];
    }
    return 0;
}
