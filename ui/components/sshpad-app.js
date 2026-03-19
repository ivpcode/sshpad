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
            configMode:    { type: String },   // NEW: 'first_run'|'locked'|'cloud'|'local'
        };
    }

    constructor() {
        super();
        this.hosts = [];
        this.tunnelStates = {};
        this.tunnelErrors = {};
        this.sseConnected = false;
        this.configMode = '';
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
        let statusData = { mode: 'local', r2Configured: false };
        try {
            const res = await fetch('/api/config/status');
            statusData = await res.json();
            this.configMode = statusData.mode;
        } catch (e) {
            console.error('Errore stato config:', e);
            this.configMode = 'local';
        }

        await this.updateComplete;

        if (this.configMode === 'first_run') {
            const wizard = this.querySelector('sshpad-wizard');
            if (wizard) {
                wizard.r2Configured = !!statusData.r2Configured;
                wizard.show();
            }
        } else if (this.configMode === 'locked') {
            const unlock = this.querySelector('sshpad-unlock');
            if (unlock) unlock.show();
        } else {
            await this._fetchHosts();
            this._connectSSE();
        }
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

    async _onWizardComplete(e) {
        this.configMode = e.detail.mode;
        await this._fetchHosts();
        this._connectSSE();
    }

    async _onUnlocked() {
        try {
            const res = await fetch('/api/config/status');
            const data = await res.json();
            this.configMode = data.mode;
        } catch (e) {
            this.configMode = 'cloud';
        }
        await this._fetchHosts();
        this._connectSSE();
    }

    async _onUseLocal() {
        this.configMode = 'local';
        await this._fetchHosts();
        this._connectSSE();
    }

    _onOpenR2Settings() {
        const r2 = this.querySelector('sshpad-r2-settings');
        if (r2) r2.show();
    }

    _onHostEdit(e) {
        const editor = this.querySelector('sshpad-host-editor');
        if (editor) editor.show(e.detail.host);
    }

    _onHostDelete(e) {
        const name = e.detail.name;
        if (!confirm(`Eliminare la connessione "${name}"?`)) return;
        this._deleteHost(name);
    }

    async _deleteHost(name) {
        try {
            const res = await this._postJSON('/api/host/delete', { name });
            const data = await res.json();
            if (!data.ok) {
                this._toast(`Errore eliminazione: ${data.error}`, 'error');
            }
            // La lista si aggiorna via SSE config_changed
        } catch (e) {
            this._toast('Errore di rete durante eliminazione', 'error');
        }
    }

    async _onHostSave(e) {
        const host = e.detail.host;
        try {
            const res = await this._postJSON('/api/host/save', host);
            const data = await res.json();
            if (data.ok) {
                this._toast('Connessione salvata', 'success', 3000);
            } else {
                this._toast(`Errore salvataggio: ${data.error}`, 'error');
            }
            // La lista si aggiorna via SSE config_changed
        } catch (e) {
            this._toast('Errore di rete durante salvataggio', 'error');
        }
    }

    _openEditor() {
        const editor = this.querySelector('sshpad-host-editor');
        if (editor) editor.showNew();
    }

    _openR2Settings() {
        const r2 = this.querySelector('sshpad-r2-settings');
        if (r2) r2.show();
    }

    /* --- Render --- */

    render() {
        const isEditable = (this.configMode === 'cloud' || this.configMode === 'local');
        const modeBadge = this.configMode === 'cloud'
            ? html`<span class="badge bg-primary ms-2">Cloud</span>`
            : this.configMode === 'local'
            ? html`<span class="badge bg-secondary ms-2">Locale</span>`
            : '';

        return html`
            <nav class="navbar fixed-top bg-white border-bottom px-3" style="z-index: 1030">
                <div class="container-fluid">
                    <span class="navbar-brand d-flex align-items-center gap-2 mb-0">
                        <img src="/terminal.svg" alt="" width="24" height="24">
                        <span class="fw-semibold">SSHPad</span>
                        ${modeBadge}
                    </span>
                    <span class="d-flex align-items-center gap-2">
                        ${isEditable ? html`
                            <button class="btn btn-outline-primary btn-sm"
                                    @click=${this._openEditor}
                                    title="Nuova connessione">
                                <i class="bi bi-plus-lg"></i>
                            </button>
                        ` : ''}
                        <button class="btn btn-outline-secondary btn-sm"
                                @click=${this._openR2Settings}
                                title="Impostazioni R2">
                            <i class="bi bi-gear"></i>
                        </button>
                        <sshpad-status-dot status="${this.sseConnected ? 'active' : 'error'}" size="md"></sshpad-status-dot>
                        <small class="text-muted font-monospace" style="font-size: 0.75rem">:${location.port || (location.protocol === 'https:' ? '443' : '80')}</small>
                    </span>
                </div>
            </nav>

            <div class="container-fluid px-3 pb-2" style="margin-top: 64px">
                ${this.hosts.length === 0 && isEditable ? html`
                    <div class="text-center text-muted py-5">
                        <p>Nessuna connessione. Clicca <strong>+</strong> per aggiungere.</p>
                    </div>
                ` : this.hosts.length === 0 ? html`
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
                                    ?editable=${isEditable}
                                    @tunnel-start=${this._onTunnelStart}
                                    @tunnel-stop=${this._onTunnelStop}
                                    @terminal-open=${this._onTerminalOpen}
                                    @host-edit=${this._onHostEdit}
                                    @host-delete=${this._onHostDelete}>
                                </sshpad-card>
                            </div>
                        `)}
                    </div>
                `}
            </div>

            <sshpad-wizard @wizard-complete=${this._onWizardComplete}></sshpad-wizard>
            <sshpad-unlock
                @unlocked=${this._onUnlocked}
                @use-local=${this._onUseLocal}
                @open-r2-settings=${this._onOpenR2Settings}>
            </sshpad-unlock>
            <sshpad-host-editor @host-save=${this._onHostSave}></sshpad-host-editor>
            <sshpad-r2-settings></sshpad-r2-settings>
            <sshpad-password></sshpad-password>
            <sshpad-toast></sshpad-toast>
        `;
    }
}

SshpadApp.RegisterElement();
