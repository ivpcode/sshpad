import { html } from 'lit';
import IVPLitElementBase from '../libs/lit-element-base.js';

class SshpadForwards extends IVPLitElementBase {

    static get tag() { return 'sshpad-forwards'; }

    static get properties() {
        return {
            forwards: { type: Array },
            status: { type: String },
            expanded: { type: Boolean, state: true },
        };
    }

    constructor() {
        super();
        this.forwards = [];
        this.status = '';
        this.expanded = false;
    }

    _toggle() {
        this.expanded = !this.expanded;
    }

    render() {
        if (!this.forwards || this.forwards.length === 0) {
            return html``;
        }

        return html`
            <!-- Header cliccabile stile VS Code tree -->
            <div class="d-flex align-items-center gap-1 sshpad-tree-header"
                 style="cursor: pointer; user-select: none; padding: 2px 0"
                 @click=${this._toggle}>
                <i class="bi bi-chevron-right"
                   style="font-size: 0.55rem; transition: transform 0.15s ease; transform: rotate(${this.expanded ? '90deg' : '0deg'}); color: #6c757d; flex-shrink: 0">
                </i>
                <span style="font-size: 0.78rem; color: #6c757d">
                    Porte mappate
                    <span class="badge rounded-pill bg-secondary bg-opacity-25 text-secondary" style="font-size: 0.65rem; font-weight: 600; vertical-align: middle">
                        ${this.forwards.length}
                    </span>
                </span>
            </div>

            <!-- Lista forward (collassabile) -->
            ${this.expanded ? html`
                <div style="padding-left: 0.6rem; border-left: 1px solid #dee2e6; margin-left: 0.25rem">
                    ${this.forwards.map(f => html`
                        <div class="d-flex justify-content-between align-items-center" style="font-size: 0.78rem; padding: 1px 0">
                            <span>
                                <span class="fw-bold text-primary" style="width: 1rem; display: inline-block">${f.type}</span>
                                <span class="text-muted">${f.label}</span>
                            </span>
                            <sshpad-status-dot status="${this.status}"></sshpad-status-dot>
                        </div>
                    `)}
                </div>
            ` : ''}
        `;
    }
}

SshpadForwards.RegisterElement();

export default SshpadForwards;
