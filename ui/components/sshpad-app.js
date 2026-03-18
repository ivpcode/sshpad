import { html } from 'lit';
import IVPLitElementBase from '../libs/lit-element-base.js';
import SSEClient from '../libs/sse-client.js';

class SshpadApp extends IVPLitElementBase {

    static get tag() { return 'sshpad-app'; }

    static get properties() {
        return {
            hosts:         { type: Array },
            tunnelStates:  { type: Object },
            tunnelErrors:  { type: Object },
            sseConnected:  { type: Boolean },
        };
    }

    constructor() {
        super();
        this.hosts = [];
        this.tunnelStates = {};
        this.tunnelErrors = {};
        this.sseConnected = false;
        this._sse = null;
    }

    connectedCallback() {
        super.connectedCallback();
        this._init();
    }

    disconnectedCallback() {
        if (this._sse) {
            this._sse.close();
            this._sse = null;
        }
        super.disconnectedCallback();
    }

    /* --- Helpers --- */

    async _postJSON(url, body) {
        const res = await fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
        });
        return res;
    }

    _clearError(host) {
        if (this.tunnelErrors[host]) {
            const errs = { ...this.tunnelErrors };
            delete errs[host];
            this.tunnelErrors = errs;
        }
    }

    _toast(message, type = 'error', duration = 6000) {
        const toast = this.querySelector('sshpad-toast');
        if (toast) toast.show(message, type, duration);
    }

    /* --- Init --- */

    async _init() {
        await this._fetchHosts();
        this._connectSSE();
    }

    async _fetchHosts() {
        try {
            const res = await fetch('/api/hosts');
            const data = await res.json();
            this.hosts = data;
            const states = { ...this.tunnelStates };
            for (const host of data) {
                if (!(host.name in states)) {
                    states[host.name] = host.tunnelStatus || 'inactive';
                }
            }
            this.tunnelStates = states;
        } catch (err) {
            console.error('Errore fetch hosts:', err);
        }
    }

    _connectSSE() {
        const sse = new SSEClient('/api/events');

        sse.onOpen = () => { this.sseConnected = true; };
        sse.onError = () => { this.sseConnected = false; };

        sse.addEventListener('tunnel_status', (data) => {
            /* Guard: skip se nulla è cambiato */
            if (this.tunnelStates[data.host] === data.status && data.status !== 'error') return;

            this.tunnelStates = { ...this.tunnelStates, [data.host]: data.status };

            if (data.status === 'error' && data.message) {
                this.tunnelErrors = { ...this.tunnelErrors, [data.host]: data.message };
                this._toast(`${data.host}: ${data.message}`, 'error');
            } else if (data.status === 'active') {
                this._clearError(data.host);
                this._toast(`${data.host}: connesso`, 'success', 3000);
            } else if (data.status === 'inactive') {
                this._clearError(data.host);
            }
        });

        sse.addEventListener('password_request', (data) => {
            const pwd = this.querySelector('sshpad-password');
            if (pwd) pwd.show(data.requestId, data.host, data.prompt);
        });

        sse.addEventListener('config_changed', () => this._fetchHosts());

        this._sse = sse;
    }

    /* --- Event handlers --- */

    async _onTunnelStart(e) {
        const host = e.detail.host;
        this.tunnelStates = { ...this.tunnelStates, [host]: 'starting' };
        try {
            await this._postJSON('/api/tunnel/start', { host });
        } catch (err) {
            console.error('Errore avvio tunnel:', err);
            this.tunnelStates = { ...this.tunnelStates, [host]: 'error' };
        }
    }

    async _onTunnelStop(e) {
        try {
            await this._postJSON('/api/tunnel/stop', { host: e.detail.host });
        } catch (err) {
            console.error('Errore stop tunnel:', err);
        }
    }

    async _onTerminalOpen(e) {
        try {
            await this._postJSON('/api/terminal/open', { host: e.detail.host });
        } catch (err) {
            console.error('Errore apertura terminale:', err);
        }
    }

    /* --- Render --- */

    render() {
        return html`
            <nav class="navbar fixed-top bg-white border-bottom px-3" style="z-index: 1030">
                <div class="container-fluid">
                    <span class="navbar-brand d-flex align-items-center gap-2 mb-0">
                        <img src="/terminal.svg" alt="" width="24" height="24">
                        <span class="fw-semibold">SSHPad</span>
                    </span>
                    <span class="d-flex align-items-center gap-2">
                        <sshpad-status-dot status="${this.sseConnected ? 'active' : 'error'}" size="md"></sshpad-status-dot>
                        <small class="text-muted font-monospace" style="font-size: 0.75rem">:${location.port || (location.protocol === 'https:' ? '443' : '80')}</small>
                    </span>
                </div>
            </nav>

            <div class="container-fluid px-3 pb-2" style="margin-top: 64px">
                ${this.hosts.length === 0 ? html`
                    <div class="text-center text-muted py-5">
                        <p>Nessun host trovato in <code>~/.ssh/config</code></p>
                    </div>
                ` : html`
                    <div class="row row-cols-1 row-cols-md-2 row-cols-xl-3 g-3 pb-4">
                        ${this.hosts.map(h => html`
                            <div class="col">
                                <sshpad-card
                                    .host=${h}
                                    status="${this.tunnelStates[h.name] || 'inactive'}"
                                    errorMessage="${this.tunnelErrors[h.name] || ''}"
                                    @tunnel-start=${this._onTunnelStart}
                                    @tunnel-stop=${this._onTunnelStop}
                                    @terminal-open=${this._onTerminalOpen}>
                                </sshpad-card>
                            </div>
                        `)}
                    </div>
                `}
            </div>

            <sshpad-password></sshpad-password>
            <sshpad-toast></sshpad-toast>
        `;
    }
}

SshpadApp.RegisterElement();
