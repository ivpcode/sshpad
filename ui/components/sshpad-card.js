import { html } from 'lit';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadCard extends IVPLitElementBase {

    static get tag() { return 'sshpad-card'; }

    static get properties() {
        return {
            host:         { type: Object },
            status:       { type: String },
            errorMessage: { type: String },
            editable:     { type: Boolean },
        };
    }

    constructor() {
        super();
        this.host         = {};
        this.status       = 'inactive';
        this.errorMessage = '';
        this.editable     = false;
    }

    _toggleTunnel(e) {
        const eventName = e.target.checked ? 'tunnel-start' : 'tunnel-stop';
        this.dispatchEvent(new CustomEvent(eventName, {
            detail:   { host: this.host.name },
            bubbles:  true,
            composed: true,
        }));
    }

    _edit(e) {
        e.stopPropagation();
        this.dispatchEvent(new CustomEvent('host-edit', {
            detail:   { host: this.host },
            bubbles:  true,
            composed: true,
        }));
    }

    _delete(e) {
        e.stopPropagation();
        this.dispatchEvent(new CustomEvent('host-delete', {
            detail:   { name: this.host.name },
            bubbles:  true,
            composed: true,
        }));
    }

    _openTerminal() {
        this.dispatchEvent(new CustomEvent('terminal-open', {
            detail:   { host: this.host.name },
            bubbles:  true,
            composed: true,
        }));
    }

    render() {
        const forwards = [];

        if (this.host.localForward) {
            for (const f of this.host.localForward) {
                forwards.push({ type: 'L', label: `${f.bindAddr}:${f.bindPort} \u2192 ${f.remoteHost}:${f.remotePort}` });
            }
        }
        if (this.host.remoteForward) {
            for (const f of this.host.remoteForward) {
                forwards.push({ type: 'R', label: `${f.bindAddr}:${f.bindPort} \u2192 ${f.remoteHost}:${f.remotePort}` });
            }
        }
        if (this.host.dynamicForward) {
            for (const f of this.host.dynamicForward) {
                forwards.push({ type: 'D', label: `${f.bindAddr || '*'}:${f.bindPort} (SOCKS)` });
            }
        }

        const isOn = this.status === 'active' || this.status === 'starting';

        return html`
            <div class="card shadow-sm mb-0 h-100">
                <div class="card-header bg-white border-0 d-flex justify-content-between align-items-center">
                    <span class="fw-bold d-flex align-items-center gap-2">
                        <sshpad-status-dot status="${this.status}"></sshpad-status-dot>
                        ${this.host.name}
                    </span>
                    <div class="d-flex align-items-center gap-2">
                        ${this.editable ? html`
                            <button class="btn btn-outline-primary btn-sm"
                                    @click=${this._edit}
                                    title="Modifica">
                                <i class="bi bi-pencil"></i>
                            </button>
                            <button class="btn btn-outline-danger btn-sm"
                                    @click=${this._delete}
                                    title="Elimina">
                                <i class="bi bi-trash"></i>
                            </button>
                        ` : ''}
                        <button class="btn btn-outline-success btn-sm font-monospace"
                                ?disabled=${this.status === 'starting'}
                                @click=${this._openTerminal}
                                title="Apri terminale">
                            &gt;_
                        </button>
                        <div class="form-check form-switch mb-0">
                            <input class="form-check-input" type="checkbox" role="switch"
                                   .checked=${isOn}
                                   ?disabled=${this.status === 'starting'}
                                   @change=${this._toggleTunnel}
                                   title="${isOn ? 'Disconnetti' : 'Connetti'}">
                        </div>
                    </div>
                </div>
                <div class="card-body pt-0">
                    <small class="text-muted d-block mb-2" style="font-size: 0.8rem; line-height: 1.6">
                        ${this.host.user || ''}@${this.host.hostname || this.host.name}${this.host.port && this.host.port !== 22 ? ':' + this.host.port : ''}
                        ${this.host.identityFile ? html`<br>\uD83D\uDD11 <span class="text-truncate">${this.host.identityFile}</span>` : ''}
                        ${this.host.proxyJump ? html`<br>\u2197 via ${this.host.proxyJump}` : ''}
                    </small>
                    ${forwards.length > 0 ? html`
                        <div class="border-top pt-2">
                            <sshpad-forwards .forwards=${forwards} status="${this.status}"></sshpad-forwards>
                        </div>
                    ` : ''}
                    ${this.status === 'error' && this.errorMessage ? html`
                        <div class="alert alert-danger py-1 px-2 mb-0 mt-2" style="font-size: 0.75rem">
                            ${this.errorMessage}
                        </div>
                    ` : ''}
                </div>
            </div>
        `;
    }
}

SshpadCard.RegisterElement();
