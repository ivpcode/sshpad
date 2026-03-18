# Documento di Progetto: Porting UI SSHPad a IVP + Bootstrap

## 1. Obiettivo

Riscrivere la UI di SSHPad utilizzando il framework **IVP** (basato su Lit Web Components) e **Bootstrap 5** con tema **chiaro (light)**, sostituendo l'attuale implementazione vanilla JS/CSS con tema scuro.

Il backend C (HTTP server, API REST, SSE) **non cambia**. Si riscrive solo il frontend servito dalla directory `ui/`.

---

## 2. Stato Attuale della UI

| Aspetto | Attuale |
|---------|---------|
| Framework | Vanilla JS (template literals + innerHTML) |
| CSS | Custom dark theme, variabili CSS, no framework |
| Componenti | Monolitico in `app.js` (~225 righe) |
| Build | Nessuno (file serviti direttamente da libmicrohttpd) |
| File | `index.html`, `app.js`, `style.css`, `terminal.svg` |

### Funzionalità UI esistenti
- Griglia responsiva di card (connessioni SSH da `~/.ssh/config`)
- Toggle switch per avvio/stop tunnel
- Bottone terminale con stato (disabled durante starting)
- Forward espandibili/collassabili (▶ con rotazione)
- Status dot animati (active/inactive/starting/error)
- Dialog modale password SSH (via `<dialog>` + SSE)
- Connessione SSE con auto-reconnect (3s)
- Indicatore stato connessione SSE nell'header

### API Endpoints (invariati)
| Metodo | Endpoint | Descrizione |
|--------|----------|-------------|
| GET | `/api/hosts` | Lista host SSH |
| GET | `/api/events` | Stream SSE |
| GET | `/api/status` | Stato tunnel |
| GET | `/api/internal/askpass` | Richiesta password (interno) |
| POST | `/api/tunnel/start` | Avvia tunnel `{host}` |
| POST | `/api/tunnel/stop` | Ferma tunnel `{host}` |
| POST | `/api/terminal/open` | Apri terminale `{host}` |
| POST | `/api/password` | Risposta password `{requestId, password}` |

---

## 3. Stack Target: IVP + Bootstrap

### Riferimento: bw-frontend (`/home/sysadmin/front/bw-frontend`)

| Componente | Dettaglio |
|------------|-----------|
| **Lit** | v3.3.0 — Web Components con template reattivi |
| **IVPLitElementBase** | Classe base con reactive properties, lifecycle hooks |
| **IVPRouter** | Router client-side (history/hash mode) — **non necessario** per SSHPad (SPA single-view) |
| **Bootstrap** | v5.1.3 — Grid, utilities, cards, modals, badges, switches |
| **SCSS** | Variabili custom + override Bootstrap |
| **Vite** | Build system — **valutare se necessario** o se servire file pre-compilati |
| **IVP Components** | Dialog, Toast, Loader — riutilizzabili |

### Cosa prendere da bw-frontend
```
src/libs/components/ivp-lit-element-base.js   → Classe base
src/libs/components/ivp-dialog.js             → Dialog modale
src/libs/components/ivp-loader.js             → Spinner (opzionale)
src/libs/components/ivp-toast.js              → Notifiche (opzionale)
src/libs/sse_client.js                        → Client SSE (da adattare)
src/libs/theme-manager.js                     → Solo tema light
```

### Cosa NON serve
- IVPRouter (una sola pagina)
- Store/Observable complesso (stato minimo)
- lang-manager (solo italiano)
- Tutte le sezioni/modelli specifici di bw-frontend

---

## 4. Architettura Target

### 4.1 Struttura File

```
ui/
├── index.html                  # Entry point, importa Bootstrap + componenti
├── main.js                     # Init app, registra componenti, avvia SSE
├── style.scss                  # Override Bootstrap, tema light SSHPad
├── libs/
│   ├── ivp-lit-element-base.js # Classe base (copiata da bw-frontend)
│   └── sse-client.js           # Client SSE adattato
├── components/
│   ├── sshpad-app.js           # Componente root (header + grid)
│   ├── sshpad-card.js          # Card singola connessione
│   ├── sshpad-toggle.js        # Toggle switch tunnel
│   ├── sshpad-forwards.js      # Lista forward espandibile
│   ├── sshpad-password.js      # Dialog password (Bootstrap modal)
│   └── sshpad-status-dot.js    # Indicatore stato
├── icons/
│   └── terminal.svg
└── vendor/
    ├── bootstrap.min.css       # Bootstrap 5 CSS
    └── bootstrap.bundle.min.js # Bootstrap 5 JS (per modal)
```

### 4.2 Scelta Build System

**Opzione A — Senza Vite (consigliata per semplicità)**
- Servire i file direttamente da libmicrohttpd (come ora)
- Usare Lit via CDN o bundle pre-compilato
- Bootstrap CSS/JS da file locali in `vendor/`
- Nessun passo di build, sviluppo immediato

**Opzione B — Con Vite**
- Richiede `npm run build` prima dell'uso
- Output in `ui/dist/` servito da libmicrohttpd
- Vantaggi: SCSS nativo, tree-shaking, HMR in dev
- Svantaggio: aggiunge complessità al processo di build

**Raccomandazione**: Opzione A per mantenere la semplicità del progetto. SSHPad è un'app leggera con pochi componenti; un build system è sovradimensionato.

### 4.3 Gerarchia Componenti

```
<sshpad-app>                          ← root component
├── <header>                          ← Bootstrap navbar-light
│   ├── Logo + Titolo "SSHPad"
│   └── <sshpad-status-dot>           ← stato SSE
├── <div class="container">
│   └── <div class="row g-3">         ← Bootstrap grid
│       ├── <sshpad-card host={}>     ← una per host
│       │   ├── <div class="card">    ← Bootstrap card
│       │   │   ├── .card-header
│       │   │   │   ├── Nome host + <sshpad-status-dot>
│       │   │   │   └── Azioni (terminale + toggle)
│       │   │   ├── .card-body
│       │   │   │   ├── Meta (user@host:port, key, proxy)
│       │   │   │   └── <sshpad-forwards>
│       │   │   │       ├── Prima regola (sempre visibile)
│       │   │   │       └── Altre regole (espandibili)
│       │   │   └── .card-footer (opzionale)
│       │   └── </div>
│       └── ...
└── <sshpad-password>                 ← Bootstrap modal
```

---

## 5. Tema Light — Palette Colori

### Variabili CSS (sostituiscono il tema scuro)

```scss
// SSHPad Light Theme
$sshpad-bg:              #f8f9fa;   // grigio chiarissimo (sfondo pagina)
$sshpad-surface:         #ffffff;   // bianco (card, dialog)
$sshpad-surface-hover:   #f0f2f5;   // grigio hover
$sshpad-border:          #dee2e6;   // grigio bordi (Bootstrap default)
$sshpad-text:            #212529;   // nero/grigio scuro (testo primario)
$sshpad-text-secondary:  #6c757d;   // grigio (testo secondario)
$sshpad-accent:          #0d6efd;   // blu Bootstrap primary
$sshpad-green:           #198754;   // verde Bootstrap success
$sshpad-red:             #dc3545;   // rosso Bootstrap danger
$sshpad-amber:           #ffc107;   // giallo Bootstrap warning

// Override Bootstrap
$body-bg:       $sshpad-bg;
$body-color:    $sshpad-text;
$card-bg:       $sshpad-surface;
$card-border-color: $sshpad-border;
```

### Mapping Classi Attuali → Bootstrap

| Attuale | Bootstrap Target |
|---------|-----------------|
| `.card` (custom) | `.card` + `.shadow-sm` |
| `.card-header` (custom) | `.card-header.bg-white.border-0` |
| `.card-meta` | `.card-body .text-muted small` |
| `.status-dot.active` | `.badge.rounded-pill.bg-success` o custom dot |
| `.status-dot.error` | `.badge.rounded-pill.bg-danger` |
| `.status-dot.starting` | `.badge.rounded-pill.bg-warning` + animazione |
| `.toggle` (custom) | `.form-check.form-switch` (Bootstrap native) |
| `.btn-terminal` | `.btn.btn-outline-success.btn-sm` |
| `dialog#password-dialog` | Bootstrap `.modal` |
| `.dialog-actions` | `.modal-footer` |
| `#connections` (CSS grid) | `.row.row-cols-1.row-cols-md-2.row-cols-lg-3.g-3` |
| header | `.navbar.navbar-light.bg-white.border-bottom` |

---

## 6. Dettaglio Componenti

### 6.1 `sshpad-app.js` — Componente Root

```
Responsabilità:
- Fetch hosts da /api/hosts
- Gestione connessione SSE (connect, reconnect, dispatch eventi)
- Mantiene stato globale: hosts[], tunnelStates{}
- Renderizza header + griglia di <sshpad-card>
- Gestisce evento password_request → mostra <sshpad-password>

Proprietà Lit:
- hosts: Array
- tunnelStates: Object
- sseConnected: Boolean

Eventi SSE gestiti:
- tunnel_status → aggiorna tunnelStates[host]
- password_request → dispatch custom event a <sshpad-password>
- config_changed → ri-fetch hosts
```

### 6.2 `sshpad-card.js` — Card Connessione

```
Responsabilità:
- Renderizza una singola card Bootstrap per un host SSH
- Mostra nome, meta info, forward, azioni
- Dispatch eventi: tunnel-start, tunnel-stop, terminal-open

Proprietà Lit:
- host: Object (ssh_host_t serializzato)
- status: String ('active'|'inactive'|'starting'|'error')

Template Bootstrap:
<div class="card shadow-sm border">
  <div class="card-header bg-white d-flex justify-content-between align-items-center">
    <span class="fw-bold">
      <sshpad-status-dot status="${this.status}"></sshpad-status-dot>
      ${this.host.name}
    </span>
    <div class="d-flex gap-2 align-items-center">
      <button class="btn btn-outline-success btn-sm font-monospace">
        &gt;_
      </button>
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" role="switch">
      </div>
    </div>
  </div>
  <div class="card-body pt-2">
    <small class="text-muted">
      ${user}@${hostname}:${port}
    </small>
    <sshpad-forwards .forwards=${forwards} status="${this.status}">
    </sshpad-forwards>
  </div>
</div>
```

### 6.3 `sshpad-forwards.js` — Forward Espandibili

```
Responsabilità:
- Mostra la prima regola forward sempre visibile
- Bottone espandi/collassa per le successive
- Badge tipo (L/R/D) con colore accent

Proprietà Lit:
- forwards: Array
- status: String
- expanded: Boolean (stato interno)

Template:
- Prima riga sempre visibile
- Icona ▶ (chevron Bootstrap) rotata 90° se expanded
- Righe aggiuntive in <div class="collapse show/hide">
```

### 6.4 `sshpad-password.js` — Dialog Password

```
Responsabilità:
- Modal Bootstrap per input password SSH
- Riceve evento password_request con requestId, host, prompt
- POST /api/password con la risposta
- Auto-focus su input password

Template Bootstrap Modal:
<div class="modal fade" tabindex="-1">
  <div class="modal-dialog modal-dialog-centered">
    <div class="modal-content">
      <div class="modal-header">
        <h5 class="modal-title">🔑 Autenticazione SSH</h5>
        <button class="btn-close"></button>
      </div>
      <div class="modal-body">
        <p class="text-muted">${prompt}</p>
        <input type="password" class="form-control">
      </div>
      <div class="modal-footer">
        <button class="btn btn-secondary">Annulla</button>
        <button class="btn btn-primary">Invia</button>
      </div>
    </div>
  </div>
</div>
```

### 6.5 `sshpad-status-dot.js` — Indicatore Stato

```
Proprietà Lit:
- status: String
- size: String ('sm'|'md') — default 'sm'

Mapping:
- active   → pallino verde + glow
- inactive → pallino grigio
- starting → pallino giallo + animazione pulse
- error    → pallino rosso
```

---

## 7. Piano di Implementazione

### Fase 1 — Setup (1 sessione)
- [ ] Copiare `ivp-lit-element-base.js` da bw-frontend, adattare (rimuovere dipendenze non necessarie)
- [ ] Scaricare Bootstrap 5.1.3 CSS/JS in `ui/vendor/`
- [ ] Scaricare Lit bundle standalone in `ui/vendor/`
- [ ] Creare nuovo `index.html` con import Bootstrap + Lit + componenti
- [ ] Creare `style.scss` (o `.css`) con tema light

### Fase 2 — Componenti Base (1-2 sessioni)
- [ ] `sshpad-status-dot.js` — componente più semplice, testare che Lit funziona
- [ ] `sshpad-toggle.js` — form-switch Bootstrap con eventi custom
- [ ] `sshpad-forwards.js` — lista espandibile con collapse
- [ ] `sshpad-card.js` — composizione dei precedenti

### Fase 3 — App Shell + SSE (1 sessione)
- [ ] `sshpad-app.js` — fetch hosts, connessione SSE, render griglia
- [ ] Collegare tutti gli eventi (start/stop tunnel, open terminal)
- [ ] Testare con il backend reale

### Fase 4 — Password Dialog (1 sessione)
- [ ] `sshpad-password.js` — modal Bootstrap
- [ ] Integrare con flusso SSE password_request
- [ ] Testare con host che richiede password

### Fase 5 — Polish (1 sessione)
- [ ] Animazioni (pulse per starting, transizioni)
- [ ] Stato vuoto ("Nessun host trovato")
- [ ] Responsive (mobile-friendly)
- [ ] Test cross-browser in WebKitGTK
- [ ] Rimuovere vecchi file `app.js`, `style.css`

---

## 8. Rischi e Note

| Rischio | Mitigazione |
|---------|-------------|
| **Lit via CDN/bundle in WebKitGTK** | Testare subito nella Fase 1 che i Web Components funzionino nel webview |
| **Bootstrap JS per modal** | Richiede `bootstrap.bundle.min.js`; verificare compatibilità con WebKitGTK |
| **Dimensione bundle** | Lit (~16KB) + Bootstrap CSS (~160KB) + JS (~80KB) ≈ 256KB totali — accettabile per app locale |
| **Shadow DOM vs Bootstrap** | Lit usa Shadow DOM di default, che isola il CSS. Bootstrap deve essere importato dentro ogni componente, oppure usare Lit senza Shadow DOM (`createRenderRoot() { return this; }`) |
| **SCSS senza build** | Se si sceglie Opzione A (no Vite), usare CSS puro con variabili CSS custom invece di SCSS |

### Nota critica: Shadow DOM
Bootstrap non funziona dentro Shadow DOM perché i suoi stili non penetrano il confine. Soluzioni:
1. **Consigliata**: Override `createRenderRoot()` per rendere nel light DOM (`return this;`)
2. Alternativa: Importare Bootstrap CSS in ogni componente con `<style>@import</style>`
3. Alternativa: Usare Constructable Stylesheets

---

## 9. Confronto Visivo

### Prima (tema scuro)
```
┌──────────────────────────────┐
│ 🖥 SSHPad              🟢   │  ← header scuro (#0f1117)
├──────────────────────────────┤
│ ┌──────────┐ ┌──────────┐   │
│ │🟢 server1│ │⚫ server2│   │  ← card scure (#181a24)
│ │user@host │ │user@host │   │
│ │L 8080→.. │ │          │   │
│ └──────────┘ └──────────┘   │
└──────────────────────────────┘
```

### Dopo (tema light Bootstrap)
```
┌──────────────────────────────┐
│ 🖥 SSHPad              🟢   │  ← navbar bianco, border-bottom
├──────────────────────────────┤
│ ┌──────────┐ ┌──────────┐   │  ← sfondo #f8f9fa
│ │🟢 server1│ │⚫ server2│   │  ← card bianche con shadow-sm
│ │user@host │ │user@host │   │
│ │L 8080→.. │ │          │   │
│ └──────────┘ └──────────┘   │
└──────────────────────────────┘
```

---

## 10. Dipendenze da Scaricare

| File | Versione | Fonte |
|------|----------|-------|
| `bootstrap.min.css` | 5.1.3 | Bootstrap CDN |
| `bootstrap.bundle.min.js` | 5.1.3 | Bootstrap CDN |
| `lit-all.min.js` | 3.3.0 | jsdelivr/unpkg (bundle standalone) |

Queste vanno in `ui/vendor/` e servite staticamente da libmicrohttpd.
