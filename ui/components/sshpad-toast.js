import { html } from 'lit';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadToast extends IVPLitElementBase {

    static get tag() { return 'sshpad-toast'; }

    static get properties() {
        return {
            _toasts: { type: Array, state: true },
        };
    }

    constructor() {
        super();
        this._toasts = [];
        this._counter = 0;
    }

    /**
     * Mostra un toast.
     * @param {string} message  - Testo del messaggio
     * @param {string} type     - 'error' | 'success' | 'warning' | 'info'
     * @param {number} duration - Durata in ms (default 6000, 0 = manuale)
     */
    show(message, type = 'error', duration = 6000) {
        const id = ++this._counter;
        this._toasts = [...this._toasts, { id, message, type }];

        if (duration > 0) {
            setTimeout(() => this._dismiss(id), duration);
        }
    }

    _dismiss(id) {
        this._toasts = this._toasts.filter(t => t.id !== id);
    }

    static TYPES = {
        error:   { cls: 'text-bg-danger',       icon: 'bi-exclamation-triangle-fill' },
        success: { cls: 'text-bg-success',       icon: 'bi-check-circle-fill' },
        warning: { cls: 'text-bg-warning text-dark', icon: 'bi-exclamation-circle-fill' },
        info:    { cls: 'text-bg-info',          icon: 'bi-info-circle-fill' },
    };

    _type(type) {
        return SshpadToast.TYPES[type] || SshpadToast.TYPES.error;
    }

    render() {
        return html`
            <div class="toast-container position-fixed bottom-0 end-0 p-3"
                 style="z-index: 1090">
                ${this._toasts.map(t => { const ty = this._type(t.type); return html`
                    <div class="toast show ${ty.cls}" role="alert">
                        <div class="d-flex align-items-center p-3">
                            <i class="bi ${ty.icon} me-2 flex-shrink-0"></i>
                            <div class="flex-grow-1" style="font-size: 0.85rem">
                                ${t.message}
                            </div>
                            <button type="button"
                                    class="btn-close btn-close-white ms-2 flex-shrink-0"
                                    @click=${() => this._dismiss(t.id)}>
                            </button>
                        </div>
                    </div>
                `; })}
            </div>
        `;
    }
}

SshpadToast.RegisterElement();

export default SshpadToast;
