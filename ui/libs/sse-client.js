export default class SSEClient {
    constructor(url) {
        this.url = url;
        this.source = null;
        this.handlers = {};
        this.onOpen = () => {};
        this.onError = () => {};
        this.lastPing = Date.now();
        this._reconnecting = false;
        this._connect();

        // Watchdog: reconnect if no activity for 30s
        this._watchdog = setInterval(() => {
            if (Date.now() - this.lastPing > 30000) {
                console.warn('SSE watchdog: reconnecting...');
                this._connect(1000);
            }
        }, 5000);
    }

    _connect(delay = 0) {
        if (this._reconnecting) return;
        this._reconnecting = true;

        if (this.source) {
            this.source.close();
            this.source = null;
        }

        const connect = () => {
            this._reconnecting = false;
            this.source = new EventSource(this.url);
            this.lastPing = Date.now();

            this.source.onopen = () => {
                console.info('SSE connected');
                this.lastPing = Date.now();
                this.onOpen();
            };

            this.source.onerror = () => {
                console.warn('SSE error, reconnecting in 3s...');
                this.onError();
                this._connect(3000);
            };

            // Re-attach all custom event handlers
            for (const [name, handler] of Object.entries(this.handlers)) {
                this.source.addEventListener(name, handler);
            }
        };

        if (delay > 0) {
            setTimeout(connect, delay);
        } else {
            connect();
        }
    }

    addEventListener(eventName, handler) {
        const wrapped = (evt) => {
            this.lastPing = Date.now();
            try {
                handler(JSON.parse(evt.data));
            } catch(e) {
                handler(evt.data);
            }
        };
        this.handlers[eventName] = wrapped;
        if (this.source) {
            this.source.addEventListener(eventName, wrapped);
        }
    }

    removeEventListener(eventName) {
        if (this.handlers[eventName] && this.source) {
            this.source.removeEventListener(eventName, this.handlers[eventName]);
        }
        delete this.handlers[eventName];
    }

    close() {
        clearInterval(this._watchdog);
        if (this.source) {
            this.source.close();
        }
    }
}
