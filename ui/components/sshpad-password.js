import { html } from 'lit';
import { Modal } from 'bootstrap';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadPassword extends IVPLitElementBase {

    static get tag() { return 'sshpad-password'; }

    static get properties() {
        return {
            requestId: { type: String },
            host: { type: String },
            prompt: { type: String },
        };
    }

    constructor() {
        super();
        this.requestId = null;
        this.host = '';
        this.prompt = '';
    }

    async show(requestId, host, prompt) {
        this.requestId = requestId;
        this.host = host;
        this.prompt = prompt;

        await this.updateComplete;
        const modalEl = this.querySelector('#password-modal');
        const modal = Modal.getOrCreateInstance(modalEl);
        modal.show();

        modalEl.addEventListener('shown.bs.modal', () => {
            this.querySelector('#password-input').focus();
        }, { once: true });
    }

    hide() {
        const modalEl = this.querySelector('#password-modal');
        const modal = Modal.getInstance(modalEl);
        if (modal) modal.hide();
        const input = this.querySelector('#password-input');
        if (input) input.value = '';
        this.requestId = null;
    }

    async _submit() {
        const input = this.querySelector('#password-input');
        const password = input?.value || '';
        if (!this.requestId) return;

        try {
            await fetch('/api/password', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ requestId: this.requestId, password })
            });
        } catch (e) {
            console.error('Errore invio password:', e);
        }
        this.hide();
    }

    _cancel() {
        this.hide();
    }

    render() {
        const displayPrompt = this.host
            ? `Password per ${this.host}:`
            : (this.prompt || 'Password:');

        return html`
            <div class="modal fade" id="password-modal" tabindex="-1" aria-hidden="true">
              <div class="modal-dialog modal-dialog-centered">
                <div class="modal-content">
                  <div class="modal-header">
                    <h5 class="modal-title">🔑 Autenticazione SSH</h5>
                    <button type="button" class="btn-close" @click=${this._cancel}></button>
                  </div>
                  <div class="modal-body">
                    <p class="text-muted mb-3">${displayPrompt}</p>
                    <input type="password" class="form-control" id="password-input"
                           placeholder="Password"
                           @keydown=${(e) => { if (e.key === 'Enter') this._submit(); }}>
                  </div>
                  <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" @click=${this._cancel}>Annulla</button>
                    <button type="button" class="btn btn-primary" @click=${this._submit}>Invia</button>
                  </div>
                </div>
              </div>
            </div>
        `;
    }
}

SshpadPassword.RegisterElement();

export default SshpadPassword;
