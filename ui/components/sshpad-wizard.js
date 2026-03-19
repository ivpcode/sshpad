import { html } from 'lit';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadWizard extends IVPLitElementBase {
    static get tag() { return 'sshpad-wizard'; }

    static get properties() {
        return {
            step: { type: Number, state: true },
            r2Config: { type: Object, state: true },
            testResult: { type: String, state: true },
            testError: { type: String, state: true },
            isNewSetup: { type: Boolean, state: true },
            loading: { type: Boolean, state: true },
            error: { type: String, state: true },
            visible: { type: Boolean, state: true },
            r2Configured: { type: Boolean },
        };
    }

    constructor() {
        super();
        this.step = 1;
        this.r2Config = { endpoint: '', accessKeyId: '', secretAccessKey: '', bucket: '', objectKey: 'sshpad-config.spd' };
        this.testResult = '';
        this.testError = '';
        this.isNewSetup = true;
        this.loading = false;
        this.error = '';
        this.visible = false;
        this.r2Configured = false;
    }

    show() { this.visible = true; this.step = 1; this.error = ''; }

    _chooseCloud() {
        if (this.r2Configured) {
            // Credenziali R2 già salvate: salta step 2, vai diretto a password
            this.isNewSetup = false;
            this.step = 3;
        } else {
            this.step = 2;
        }
    }

    async _chooseLocal() {
        this.loading = true;
        try {
            const res = await fetch('/api/config/setup', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ mode: 'local' })
            });
            const data = await res.json();
            if (data.ok) {
                this.visible = false;
                this.dispatchEvent(new CustomEvent('wizard-complete', {
                    detail: { mode: 'local' }, bubbles: true, composed: true
                }));
            } else {
                this.error = data.error || 'Errore durante la configurazione';
            }
        } catch(e) {
            this.error = 'Errore di rete';
        } finally {
            this.loading = false;
        }
    }

    async _testR2() {
        this.loading = true;
        this.testResult = '';
        try {
            const res = await fetch('/api/config/r2-test', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    endpoint: this.r2Config.endpoint,
                    accessKeyId: this.r2Config.accessKeyId,
                    secretAccessKey: this.r2Config.secretAccessKey,
                    bucket: this.r2Config.bucket,
                    objectKey: this.r2Config.objectKey || 'sshpad-config.spd'
                })
            });
            const data = await res.json();
            if (data.ok) {
                this.testResult = 'ok';
                this.testError = '';
            } else {
                this.testResult = 'error';
                this.testError = data.error || 'Connessione fallita';
            }
        } catch(e) {
            this.testResult = 'error';
            this.testError = 'Errore di rete';
        } finally {
            this.loading = false;
        }
    }

    async _goToStep3() {
        // Determina se è primo setup (nessun file esistente su R2 = isNewSetup = true)
        // Per semplicità, isNewSetup = true (il backend gestirà 404 come primo setup)
        this.isNewSetup = true;
        this.step = 3;
    }

    async _completeSetup() {
        const pwdInput = this.querySelector('#wizard-password');
        const password = pwdInput?.value || '';

        if (!password) {
            this.error = 'Inserisci una password';
            return;
        }

        if (this.isNewSetup) {
            const confirmInput = this.querySelector('#wizard-password-confirm');
            const confirm = confirmInput?.value || '';
            if (password !== confirm) {
                this.error = 'Le password non corrispondono';
                return;
            }
        }

        this.loading = true;
        this.error = '';

        try {
            const res = await fetch('/api/config/setup', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    mode: 'cloud',
                    r2: {
                        endpoint: this.r2Config.endpoint,
                        accessKeyId: this.r2Config.accessKeyId,
                        secretAccessKey: this.r2Config.secretAccessKey,
                        bucket: this.r2Config.bucket,
                        objectKey: this.r2Config.objectKey || 'sshpad-config.spd'
                    },
                    password
                })
            });
            const data = await res.json();
            if (data.ok) {
                this.visible = false;
                this.dispatchEvent(new CustomEvent('wizard-complete', {
                    detail: { mode: 'cloud' }, bubbles: true, composed: true
                }));
            } else if (data.error === 'bad_password') {
                this.error = 'Password errata';
                this.isNewSetup = false; // file esiste, è una seconda macchina
                if (pwdInput) pwdInput.value = '';
            } else {
                this.error = data.error || 'Errore durante la configurazione';
            }
        } catch(e) {
            this.error = 'Errore di rete';
        } finally {
            this.loading = false;
        }
    }

    _renderStep1() {
        return html`
            <div class="text-center mb-4">
                <h2 class="fw-bold">Benvenuto in SSHPad</h2>
                <p class="text-muted">Come vuoi gestire le tue connessioni SSH?</p>
            </div>
            <div class="row g-4 justify-content-center">
                <div class="col-md-5">
                    <div class="card h-100 border-2 cursor-pointer" style="cursor:pointer"
                         @click=${this._chooseCloud}>
                        <div class="card-body text-center p-4">
                            <div style="font-size:3rem">&#x2601;&#xFE0F;</div>
                            <h5 class="card-title mt-2">Sincronizzazione Cloud</h5>
                            <p class="card-text text-muted">Le connessioni SSH vengono salvate crittografate su Cloudflare R2 e sincronizzate tra tutte le tue macchine.</p>
                            <p class="text-muted small">Richiede un account Cloudflare con bucket R2.</p>
                            <button class="btn btn-primary mt-2">Configura &rarr;</button>
                        </div>
                    </div>
                </div>
                <div class="col-md-5">
                    <div class="card h-100 border-2" style="cursor:pointer"
                         @click=${this._chooseLocal}>
                        <div class="card-body text-center p-4">
                            <div style="font-size:3rem">&#x1F4BB;</div>
                            <h5 class="card-title mt-2">Solo Locale</h5>
                            <p class="card-text text-muted">Le connessioni SSH vengono salvate in ~/.ssh/config solo su questa macchina.</p>
                            <p class="text-muted small">Nessuna configurazione aggiuntiva richiesta.</p>
                            <button class="btn btn-outline-secondary mt-2">Inizia &rarr;</button>
                        </div>
                    </div>
                </div>
            </div>
            ${this.error ? html`<div class="alert alert-danger mt-3">${this.error}</div>` : ''}
        `;
    }

    _renderStep2() {
        return html`
            <div class="mb-3">
                <button class="btn btn-link text-decoration-none ps-0" @click=${() => this.step = 1}>&larr; Indietro</button>
            </div>
            <h4 class="mb-3">Configurazione Cloudflare R2</h4>
            ${this.testResult === 'error' ? html`<div class="alert alert-danger">${this.testError}</div>` : ''}
            ${this.testResult === 'ok' ? html`<div class="alert alert-success">&#x2713; Connessione riuscita</div>` : ''}

            <div class="mb-3">
                <label class="form-label">Endpoint URL</label>
                <input type="text" class="form-control" placeholder="https://<account>.r2.cloudflarestorage.com"
                       .value=${this.r2Config.endpoint}
                       @input=${e => this.r2Config = {...this.r2Config, endpoint: e.target.value}}>
            </div>
            <div class="mb-3">
                <label class="form-label">Access Key ID</label>
                <input type="text" class="form-control"
                       .value=${this.r2Config.accessKeyId}
                       @input=${e => this.r2Config = {...this.r2Config, accessKeyId: e.target.value}}>
            </div>
            <div class="mb-3">
                <label class="form-label">Secret Access Key</label>
                <input type="password" class="form-control"
                       .value=${this.r2Config.secretAccessKey}
                       @input=${e => this.r2Config = {...this.r2Config, secretAccessKey: e.target.value}}>
            </div>
            <div class="mb-3">
                <label class="form-label">Bucket</label>
                <input type="text" class="form-control"
                       .value=${this.r2Config.bucket}
                       @input=${e => this.r2Config = {...this.r2Config, bucket: e.target.value}}>
            </div>
            <div class="mb-3">
                <label class="form-label">Object Key</label>
                <input type="text" class="form-control"
                       .value=${this.r2Config.objectKey}
                       @input=${e => this.r2Config = {...this.r2Config, objectKey: e.target.value}}>
            </div>

            <div class="d-flex gap-2 justify-content-end">
                <button class="btn btn-outline-secondary" @click=${this._testR2} ?disabled=${this.loading}>
                    ${this.loading ? html`<span class="spinner-border spinner-border-sm me-1"></span>` : ''}
                    Testa Connessione
                </button>
                <button class="btn btn-primary" @click=${this._goToStep3}
                        ?disabled=${this.testResult !== 'ok'}>
                    Avanti &rarr;
                </button>
            </div>
        `;
    }

    _renderStep3() {
        return html`
            <div class="mb-3">
                <button class="btn btn-link text-decoration-none ps-0"
                        @click=${() => this.step = this.r2Configured ? 1 : 2}>&larr; Indietro</button>
            </div>
            <h4 class="mb-3">&#x1F510; Password Master</h4>
            <p class="text-muted">
                ${this.isNewSetup
                    ? 'Crea una password master per proteggere le connessioni.'
                    : 'Inserisci la password master per sbloccare le connessioni.'}
            </p>

            ${this.error ? html`<div class="alert alert-danger">${this.error}</div>` : ''}

            <div class="mb-3">
                <label class="form-label">Password</label>
                <input type="password" class="form-control" id="wizard-password"
                       @keydown=${e => e.key === 'Enter' && this._completeSetup()}>
            </div>
            ${this.isNewSetup ? html`
                <div class="mb-3">
                    <label class="form-label">Conferma Password</label>
                    <input type="password" class="form-control" id="wizard-password-confirm"
                           @keydown=${e => e.key === 'Enter' && this._completeSetup()}>
                </div>
            ` : ''}

            <div class="alert alert-warning small">
                &#x26A0;&#xFE0F; Questa password non è recuperabile. Conservala in un posto sicuro.
            </div>

            <div class="d-flex justify-content-end">
                <button class="btn btn-primary" @click=${this._completeSetup} ?disabled=${this.loading}>
                    ${this.loading ? html`<span class="spinner-border spinner-border-sm me-1"></span>` : ''}
                    Completa &rarr;
                </button>
            </div>
        `;
    }

    render() {
        if (!this.visible) return html``;

        return html`
            <div style="position:fixed;inset:0;z-index:1060;background:var(--bs-body-bg,#fff);overflow-y:auto">
                <div class="container py-5" style="max-width:900px">
                    <div class="text-center mb-4">
                        <img src="/terminal.svg" alt="SSHPad" width="48" height="48">
                        <span class="fs-4 fw-bold ms-2">SSHPad</span>
                    </div>

                    ${this.step === 1 ? this._renderStep1() : ''}
                    ${this.step === 2 ? this._renderStep2() : ''}
                    ${this.step === 3 ? this._renderStep3() : ''}
                </div>
            </div>
        `;
    }
}

SshpadWizard.RegisterElement();
export default SshpadWizard;
