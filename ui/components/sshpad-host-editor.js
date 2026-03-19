import { html } from 'lit';
import { Modal } from 'bootstrap';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadHostEditor extends IVPLitElementBase {
    static get tag() { return 'sshpad-host-editor'; }

    static get properties() {
        return {
            _editMode: { type: Boolean, state: true },
            _host: { type: Object, state: true },
            _localForwards: { type: Array, state: true },
            _remoteForwards: { type: Array, state: true },
            _dynamicForwards: { type: Array, state: true },
            _error: { type: String, state: true },
        };
    }

    constructor() {
        super();
        this._editMode = false;
        this._host = {};
        this._localForwards = [];
        this._remoteForwards = [];
        this._dynamicForwards = [];
        this._error = '';
    }

    _emptyHost() {
        return { name: '', hostname: '', user: '', port: 22, identityFile: '', proxyJump: '' };
    }

    async show(host) {
        this._editMode = true;
        this._host = { ...this._emptyHost(), ...host };
        this._localForwards = [...(host.localForward || [])].map(f => ({...f}));
        this._remoteForwards = [...(host.remoteForward || [])].map(f => ({...f}));
        this._dynamicForwards = [...(host.dynamicForward || [])].map(f => ({...f}));
        this._error = '';
        await this.updateComplete;
        this._openModal();
    }

    async showNew() {
        this._editMode = false;
        this._host = this._emptyHost();
        this._localForwards = [];
        this._remoteForwards = [];
        this._dynamicForwards = [];
        this._error = '';
        await this.updateComplete;
        this._openModal();
    }

    _openModal() {
        const modalEl = this.querySelector('#host-editor-modal');
        if (modalEl) {
            Modal.getOrCreateInstance(modalEl).show();
            modalEl.addEventListener('shown.bs.modal', () => {
                this.querySelector('#editor-name')?.focus();
            }, { once: true });
        }
    }

    _close() {
        const modalEl = this.querySelector('#host-editor-modal');
        const modal = Modal.getInstance(modalEl);
        if (modal) modal.hide();
    }

    _save() {
        const name = this.querySelector('#editor-name')?.value?.trim() || '';
        if (!name) { this._error = 'Il campo Host alias è obbligatorio'; return; }

        // Leggi tutti i campi del form
        const getVal = id => this.querySelector(`#${id}`)?.value?.trim() || '';
        const getInt = id => parseInt(this.querySelector(`#${id}`)?.value || '22', 10) || 22;

        const host = {
            name,
            hostname: getVal('editor-hostname'),
            user: getVal('editor-user'),
            port: getInt('editor-port'),
            identityFile: getVal('editor-identity'),
            proxyJump: getVal('editor-proxy'),
            localForward: this._localForwards.filter(f => f.bindPort && f.remoteHost && f.remotePort),
            remoteForward: this._remoteForwards.filter(f => f.bindPort && f.remoteHost && f.remotePort),
            dynamicForward: this._dynamicForwards.filter(f => f.bindPort),
        };

        this.dispatchEvent(new CustomEvent('host-save', {
            detail: { host },
            bubbles: true,
            composed: true,
        }));
        this._close();
    }

    // Forward management helpers
    _addLocalFwd()    { this._localForwards   = [...this._localForwards,   { bindAddr: 'localhost', bindPort: '', remoteHost: '', remotePort: '' }]; }
    _removeLF(i)      { this._localForwards   = this._localForwards.filter((_, idx) => idx !== i); }
    _addRemoteFwd()   { this._remoteForwards  = [...this._remoteForwards,  { bindAddr: 'localhost', bindPort: '', remoteHost: '', remotePort: '' }]; }
    _removeRF(i)      { this._remoteForwards  = this._remoteForwards.filter((_, idx) => idx !== i); }
    _addDynamicFwd()  { this._dynamicForwards = [...this._dynamicForwards, { bindAddr: 'localhost', bindPort: '' }]; }
    _removeDF(i)      { this._dynamicForwards = this._dynamicForwards.filter((_, idx) => idx !== i); }

    _updateLF(i, field, val) {
        const arr = this._localForwards.map((f, idx) => idx === i ? {...f, [field]: val} : f);
        this._localForwards = arr;
    }
    _updateRF(i, field, val) {
        const arr = this._remoteForwards.map((f, idx) => idx === i ? {...f, [field]: val} : f);
        this._remoteForwards = arr;
    }
    _updateDF(i, field, val) {
        const arr = this._dynamicForwards.map((f, idx) => idx === i ? {...f, [field]: val} : f);
        this._dynamicForwards = arr;
    }

    _renderForwardRow(fwd, index, type) {
        if (type === 'dynamic') {
            return html`
                <div class="row g-2 mb-2 align-items-center">
                    <div class="col">
                        <input type="text" class="form-control form-control-sm" placeholder="localhost"
                               .value=${fwd.bindAddr || ''}
                               @input=${e => this._updateDF(index, 'bindAddr', e.target.value)}>
                    </div>
                    <div class="col-3">
                        <input type="number" class="form-control form-control-sm" placeholder="Porta"
                               .value=${fwd.bindPort || ''}
                               @input=${e => this._updateDF(index, 'bindPort', parseInt(e.target.value)||0)}>
                    </div>
                    <div class="col-auto">
                        <button type="button" class="btn btn-outline-danger btn-sm"
                                @click=${() => this._removeDF(index)}>×</button>
                    </div>
                </div>
            `;
        }
        const updateFn = type === 'local' ? this._updateLF.bind(this) : this._updateRF.bind(this);
        return html`
            <div class="row g-1 mb-2 align-items-center">
                <div class="col-3">
                    <input type="text" class="form-control form-control-sm" placeholder="Bind addr"
                           .value=${fwd.bindAddr || ''}
                           @input=${e => updateFn(index, 'bindAddr', e.target.value)}>
                </div>
                <div class="col-2">
                    <input type="number" class="form-control form-control-sm" placeholder="Porta"
                           .value=${fwd.bindPort || ''}
                           @input=${e => updateFn(index, 'bindPort', parseInt(e.target.value)||0)}>
                </div>
                <div class="col-4">
                    <input type="text" class="form-control form-control-sm" placeholder="Remote host"
                           .value=${fwd.remoteHost || ''}
                           @input=${e => updateFn(index, 'remoteHost', e.target.value)}>
                </div>
                <div class="col-2">
                    <input type="number" class="form-control form-control-sm" placeholder="Porta"
                           .value=${fwd.remotePort || ''}
                           @input=${e => updateFn(index, 'remotePort', parseInt(e.target.value)||0)}>
                </div>
                <div class="col-1">
                    <button type="button" class="btn btn-outline-danger btn-sm"
                            @click=${() => type === 'local' ? this._removeLF(index) : this._removeRF(index)}>×</button>
                </div>
            </div>
        `;
    }

    render() {
        const title = this._editMode ? `Modifica: ${this._host.name}` : 'Nuova Connessione';

        return html`
            <div class="modal fade" id="host-editor-modal" tabindex="-1" aria-hidden="true">
                <div class="modal-dialog modal-lg modal-dialog-scrollable">
                    <div class="modal-content">
                        <div class="modal-header">
                            <h5 class="modal-title">${title}</h5>
                            <button type="button" class="btn-close" @click=${this._close}></button>
                        </div>
                        <div class="modal-body">
                            ${this._error ? html`<div class="alert alert-danger">${this._error}</div>` : ''}

                            <div class="row g-3">
                                <div class="col-md-6">
                                    <label class="form-label">Host alias *</label>
                                    <input type="text" class="form-control" id="editor-name"
                                           .value=${this._host.name || ''}
                                           ?readonly=${this._editMode}
                                           placeholder="es. myserver">
                                </div>
                                <div class="col-md-6">
                                    <label class="form-label">HostName</label>
                                    <input type="text" class="form-control" id="editor-hostname"
                                           .value=${this._host.hostname || ''}
                                           placeholder="es. 192.168.1.1">
                                </div>
                                <div class="col-md-4">
                                    <label class="form-label">User</label>
                                    <input type="text" class="form-control" id="editor-user"
                                           .value=${this._host.user || ''}
                                           placeholder="es. admin">
                                </div>
                                <div class="col-md-2">
                                    <label class="form-label">Port</label>
                                    <input type="number" class="form-control" id="editor-port"
                                           .value=${this._host.port || 22}
                                           min="1" max="65535">
                                </div>
                                <div class="col-md-6">
                                    <label class="form-label">IdentityFile</label>
                                    <input type="text" class="form-control" id="editor-identity"
                                           .value=${this._host.identityFile || ''}
                                           placeholder="es. ~/.ssh/id_rsa">
                                </div>
                                <div class="col-12">
                                    <label class="form-label">ProxyJump</label>
                                    <input type="text" class="form-control" id="editor-proxy"
                                           .value=${this._host.proxyJump || ''}
                                           placeholder="es. bastion">
                                </div>
                            </div>

                            <!-- Local Forwards -->
                            <div class="mt-4">
                                <div class="d-flex justify-content-between align-items-center mb-2">
                                    <label class="form-label mb-0 fw-bold">Local Forwards</label>
                                    <button type="button" class="btn btn-outline-secondary btn-sm"
                                            @click=${this._addLocalFwd}>+ Aggiungi</button>
                                </div>
                                ${this._localForwards.length > 0 ? html`
                                    <div class="row g-1 mb-1 text-muted" style="font-size:0.75rem">
                                        <div class="col-3">Bind addr</div>
                                        <div class="col-2">Porta</div>
                                        <div class="col-4">Remote host</div>
                                        <div class="col-2">Porta</div>
                                    </div>
                                ` : ''}
                                ${this._localForwards.map((f, i) => this._renderForwardRow(f, i, 'local'))}
                            </div>

                            <!-- Remote Forwards -->
                            <div class="mt-3">
                                <div class="d-flex justify-content-between align-items-center mb-2">
                                    <label class="form-label mb-0 fw-bold">Remote Forwards</label>
                                    <button type="button" class="btn btn-outline-secondary btn-sm"
                                            @click=${this._addRemoteFwd}>+ Aggiungi</button>
                                </div>
                                ${this._remoteForwards.map((f, i) => this._renderForwardRow(f, i, 'remote'))}
                            </div>

                            <!-- Dynamic Forwards -->
                            <div class="mt-3">
                                <div class="d-flex justify-content-between align-items-center mb-2">
                                    <label class="form-label mb-0 fw-bold">Dynamic Forwards (SOCKS)</label>
                                    <button type="button" class="btn btn-outline-secondary btn-sm"
                                            @click=${this._addDynamicFwd}>+ Aggiungi</button>
                                </div>
                                ${this._dynamicForwards.map((f, i) => this._renderForwardRow(f, i, 'dynamic'))}
                            </div>
                        </div>
                        <div class="modal-footer">
                            <button type="button" class="btn btn-secondary" @click=${this._close}>Annulla</button>
                            <button type="button" class="btn btn-primary" @click=${this._save}>Salva</button>
                        </div>
                    </div>
                </div>
            </div>
        `;
    }
}

SshpadHostEditor.RegisterElement();
export default SshpadHostEditor;
