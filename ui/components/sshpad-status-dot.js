import { html } from 'lit';
import { styleMap } from 'lit/directives/style-map.js';
import IVPLitElementBase from '../libs/lit-element-base.js';

/* Style cache per (status, size) — evita allocazioni a ogni render */
const _styleCache = {};

function getStyles(status, size) {
    const key = `${status}-${size}`;
    if (_styleCache[key]) return _styleCache[key];

    const dim = size === 'md' ? '12px' : '8px';
    const base = {
        display: 'inline-block',
        width: dim,
        height: dim,
        borderRadius: '50%',
        flexShrink: '0',
    };

    let result;
    switch (status) {
        case 'active':
            result = { ...base, backgroundColor: '#198754', boxShadow: '0 0 6px rgba(25, 135, 84, 0.6)' };
            break;
        case 'starting':
            result = { ...base, backgroundColor: '#ffc107', animation: 'pulse 1.2s ease-in-out infinite' };
            break;
        case 'error':
            result = { ...base, backgroundColor: '#dc3545' };
            break;
        default:
            result = { ...base, backgroundColor: '#6c757d' };
            break;
    }

    _styleCache[key] = result;
    return result;
}

class SshpadStatusDot extends IVPLitElementBase {

    static get tag() { return 'sshpad-status-dot'; }

    static get properties() {
        return {
            status: { type: String },
            size: { type: String },
        };
    }

    constructor() {
        super();
        this.status = 'inactive';
        this.size = 'sm';
    }

    render() {
        return html`<span style=${styleMap(getStyles(this.status, this.size))}></span>`;
    }
}

SshpadStatusDot.RegisterElement();

export default SshpadStatusDot;
