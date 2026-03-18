import { LitElement } from 'lit';

export default class IVPLitElementBase extends LitElement {

    // Override: use light DOM so Bootstrap styles apply
    createRenderRoot() {
        return this;
    }

    // Static tag name - subclasses MUST override
    static get tag() { return 'ivp-element' }

    // Register custom element
    static RegisterElement() {
        if (!customElements.get(this.tag)) {
            customElements.define(this.tag, this);
        }
    }

    // Promise-based requestAnimationFrame
    RequestAnimationFrame() {
        return new Promise(resolve => requestAnimationFrame(resolve));
    }

    connectedCallback() {
        super.connectedCallback();
    }

    disconnectedCallback() {
        super.disconnectedCallback();
    }
}
