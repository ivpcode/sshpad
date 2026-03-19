import { html } from 'lit';
import { Modal } from 'bootstrap';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadR2Settings extends IVPLitElementBase {
    static get tag() { return 'sshpad-r2-settings'; }

    static get properties() {
        return {
            _cfg: { type: Object, state: true },
            _testResult: { type: String, state: true },  // '', 'ok', 'error'
            _testError: { type: String, state: true },
            _saveError: { type: String, state: true },
            _loading: { type: Boolean, state: true },
        };
    }

    constructor() {
        super();
        this._cfg = { endpoint: '', accessKeyId: '', secretAccessKey: '', bucket: '', objectKey: 'sshpad-config.spd' };
        this._testResult = '';
        this._testError = '';
        this._saveError = '';
        this._loading = false;
    }

    async show() {
        // Carica valori attuali
        try {
            const res = await fetch('/api/config/r2-settings');
            if (res.ok) {
                const data = await res.json();
                this._cfg = { ...this._cfg, ...data };
            }
        } catch (e) {
            console.error('Errore caricamento R2 settings:', e);
        }
        this._testResult = '';
        this._testError = '';
        this._saveError = '';
        await this.updateComplete;
        const modalEl = this.querySelector('#r2-settings-modal');
        if (modalEl) Modal.getOrCreateInstance(modalEl).show();
    }

    _close() {
        const modalEl = this.querySelector('#r2-settings-modal');
        const modal = Modal.getInstance(modalEl);
        if (modal) modal.hide();
    }

    _getFormCfg() {
        return {
            endpoint:        this.querySelector('#r2-endpoint')?.value?.trim()    || '',
            accessKeyId:     this.querySelector('#r2-access-key')?.value?.trim()  || '',
            secretAccessKey: this.querySelector('#r2-secret')?.value?.trim()      || '',
            bucket:          this.querySelector('#r2-bucket')?.value?.trim()      || '',
            objectKey:       this.querySelector('#r2-object-key')?.value?.trim()  || 'sshpad-config.spd',
        };
    }

    async _test() {
        const cfg = this._getFormCfg();
        this._loading = true;
        this._testResult = '';
        try {
            const res = await fetch('/api/config/r2-test', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(cfg)
            });
            const data = await res.json();
            if (data.ok) {
                this._testResult = 'ok';
                this._testError = '';
            } else {
                this._testResult = 'error';
                this._testError = data.error || 'Connessione fallita';
            }
        } catch (e) {
            this._testResult = 'error';
            this._testError = 'Errore di rete';
        } finally {
            this._loading = false;
        }
    }

    async _save() {
        const cfg = this._getFormCfg();
        this._loading = true;
        this._saveError = '';
        try {
            const res = await fetch('/api/config/r2-settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(cfg)
            });
            const data = await res.json();
            if (data.ok) {
                this._close();
                this.dispatchEvent(new CustomEvent('r2-saved', { bubbles: true, composed: true }));
            } else {
                this._saveError = data.error || 'Errore durante il salvataggio';
            }
        } catch (e) {
            this._saveError = 'Errore di rete';
        } finally {
            this._loading = false;
        }
    }

    render() {
        return html`
            <div class="modal fade" id="r2-settings-modal" tabindex="-1" aria-hidden="true"
                 aria-labelledby="r2-settings-title">
                <div class="modal-dialog modal-dialog-centered">
                    <div class="modal-content">
                        <div class="modal-header">
                            <h5 class="modal-title" id="r2-settings-title">⚙️ Impostazioni Cloudflare R2</h5>
                            <button type="button" class="btn-close" @click=${this._close}></button>
                        </div>
                        <div class="modal-body">
                            ${this._testResult === 'ok' ? html`
                                <div class="alert alert-success py-2 mb-3">✓ Connessione riuscita</div>
                            ` : ''}
                            ${this._testResult === 'error' ? html`
                                <div class="alert alert-danger py-2 mb-3">${this._testError}</div>
                            ` : ''}
                            ${this._saveError ? html`
                                <div class="alert alert-danger py-2 mb-3">${this._saveError}</div>
                            ` : ''}

                            <div class="mb-3">
                                <label class="form-label">Endpoint URL</label>
                                <input type="text" class="form-control" id="r2-endpoint"
                                       .value=${this._cfg.endpoint || ''}
                                       placeholder="https://<account>.r2.cloudflarestorage.com">
                            </div>
                            <div class="mb-3">
                                <label class="form-label">Access Key ID</label>
                                <input type="text" class="form-control" id="r2-access-key"
                                       .value=${this._cfg.accessKeyId || ''}>
                            </div>
                            <div class="mb-3">
                                <label class="form-label">Secret Access Key</label>
                                <input type="password" class="form-control" id="r2-secret"
                                       .value=${this._cfg.secretAccessKey || ''}
                                       placeholder="Lascia vuoto per mantenere il valore attuale">
                            </div>
                            <div class="mb-3">
                                <label class="form-label">Bucket</label>
                                <input type="text" class="form-control" id="r2-bucket"
                                       .value=${this._cfg.bucket || ''}>
                            </div>
                            <div class="mb-3">
                                <label class="form-label">Object Key</label>
                                <input type="text" class="form-control" id="r2-object-key"
                                       .value=${this._cfg.objectKey || 'sshpad-config.spd'}>
                            </div>
                        </div>
                        <div class="modal-footer d-flex justify-content-between">
                            <button type="button" class="btn btn-outline-secondary"
                                    @click=${this._test} ?disabled=${this._loading}>
                                ${this._loading ? html`<span class="spinner-border spinner-border-sm me-1"></span>` : ''}
                                Testa Connessione
                            </button>
                            <div>
                                <button type="button" class="btn btn-secondary me-2"
                                        @click=${this._close}>Annulla</button>
                                <button type="button" class="btn btn-primary"
                                        @click=${this._save} ?disabled=${this._loading}>
                                    Salva
                                </button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        `;
    }
}

SshpadR2Settings.RegisterElement();
export default SshpadR2Settings;
