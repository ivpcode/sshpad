import { html } from 'lit';
import { Modal } from 'bootstrap';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadUnlock extends IVPLitElementBase {
    static get tag() { return 'sshpad-unlock'; }

    static get properties() {
        return {
            loading: { type: Boolean, state: true },
            error: { type: String, state: true },
        };
    }

    constructor() {
        super();
        this.loading = false;
        this.error = '';
    }

    async show() {
        await this.updateComplete;
        const modalEl = this.querySelector('#unlock-modal');
        const modal = Modal.getOrCreateInstance(modalEl, {
            backdrop: 'static',
            keyboard: false,
        });
        modal.show();
        modalEl.addEventListener('shown.bs.modal', () => {
            this.querySelector('#unlock-password')?.focus();
        }, { once: true });
    }

    hide() {
        const modalEl = this.querySelector('#unlock-modal');
        const modal = Modal.getInstance(modalEl);
        if (modal) modal.hide();
        const input = this.querySelector('#unlock-password');
        if (input) input.value = '';
        this.error = '';
        this.loading = false;
    }

    async _submit() {
        const input = this.querySelector('#unlock-password');
        const password = input?.value || '';
        if (!password) return;

        this.loading = true;
        this.error = '';

        try {
            const res = await fetch('/api/config/unlock', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ password })
            });
            const data = await res.json();
            if (data.ok) {
                this.hide();
                this.dispatchEvent(new CustomEvent('unlocked', { bubbles: true, composed: true }));
            } else if (data.error === 'bad_password') {
                this.error = 'Password errata. Riprova.';
                if (input) { input.value = ''; input.focus(); }
            } else {
                this.error = data.error === 'network_error'
                    ? 'Impossibile raggiungere il cloud. Riprova o usa la configurazione locale.'
                    : (data.error || 'Errore sconosciuto');
            }
        } catch (e) {
            this.error = 'Errore di rete';
        } finally {
            this.loading = false;
        }
    }

    async _useLocal() {
        this.loading = true;
        try {
            await fetch('/api/config/use-local', { method: 'POST' });
            this.hide();
            this.dispatchEvent(new CustomEvent('use-local', { bubbles: true, composed: true }));
        } catch (e) {
            this.error = 'Errore di rete';
        } finally {
            this.loading = false;
        }
    }

    _openR2Settings() {
        // Emetti evento per aprire il dialog R2 settings
        this.dispatchEvent(new CustomEvent('open-r2-settings', { bubbles: true, composed: true }));
    }

    render() {
        return html`
            <div class="modal fade" id="unlock-modal" tabindex="-1"
                 data-bs-backdrop="static" data-bs-keyboard="false"
                 aria-hidden="true" aria-labelledby="unlock-modal-title">
                <div class="modal-dialog modal-dialog-centered">
                    <div class="modal-content">
                        <div class="modal-header border-0 pb-0">
                            <h5 class="modal-title" id="unlock-modal-title">🔐 Sblocca SSHPad</h5>
                        </div>
                        <div class="modal-body">
                            <p class="text-muted mb-3">Inserisci la password master per accedere alle connessioni SSH.</p>
                            ${this.error ? html`
                                <div class="alert alert-danger py-2 px-3 mb-3" style="font-size:0.875rem">
                                    ${this.error}
                                </div>
                            ` : ''}
                            <input type="password" class="form-control mb-1" id="unlock-password"
                                   placeholder="Password master"
                                   @keydown=${e => { if (e.key === 'Enter') this._submit(); }}>
                        </div>
                        <div class="modal-footer d-flex flex-column align-items-stretch gap-2 border-0 pt-0">
                            <button type="button" class="btn btn-primary" @click=${this._submit}
                                    ?disabled=${this.loading}>
                                ${this.loading ? html`<span class="spinner-border spinner-border-sm me-2"></span>` : ''}
                                Sblocca
                            </button>
                            <button type="button" class="btn btn-outline-secondary"
                                    @click=${this._useLocal} ?disabled=${this.loading}>
                                Usa configurazione locale
                            </button>
                            <button type="button" class="btn btn-link text-muted btn-sm"
                                    @click=${this._openR2Settings}>
                                Impostazioni R2
                            </button>
                        </div>
                    </div>
                </div>
            </div>
        `;
    }
}

SshpadUnlock.RegisterElement();
export default SshpadUnlock;
