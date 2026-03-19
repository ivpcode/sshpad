# SSHPad — Configurazione SSH Portatile Crittografata su Cloudflare R2

## Contesto

Attualmente SSHPad legge `~/.ssh/config` in sola lettura. La configurazione non è portabile tra macchine diverse. L'obiettivo è salvare la configurazione SSH come file crittografato su Cloudflare R2 (S3-compatible), permettendo di scaricarla, decriptarla con password master, editarla via UI, e sincronizzarla automaticamente.

## Modalità operative

| Modalità | Quando | Editing |
|----------|--------|---------|
| **FIRST_RUN** | Primo avvio (nessun `r2.json`) | No — wizard obbligatorio |
| **LOCKED** | R2 configurato, password non ancora inserita | No |
| **CLOUD** | R2 sbloccato, config dal cloud | Sì (CRUD completo) |
| **LOCAL** | R2 non configurato o irraggiungibile | Sì (CRUD su ~/.ssh/config) |

---

## Wizard di primo avvio

Al primo avvio in assoluto (nessun `~/.config/sshpad/r2.json` presente), SSHPad mostra un wizard a schermo intero prima di qualsiasi altra cosa.

### Step 1 — Scelta modalità

Schermata con due card grandi centrate:

```
┌─────────────────────────────────┐   ┌─────────────────────────────────┐
│  ☁️  Sincronizzazione Cloud     │   │  💻  Solo Locale                │
│                                 │   │                                 │
│  Le connessioni SSH vengono     │   │  Le connessioni SSH vengono     │
│  salvate crittografate su       │   │  salvate in ~/.ssh/config       │
│  Cloudflare R2 e sincronizzate  │   │  solo su questa macchina.       │
│  tra tutte le tue macchine.     │   │                                 │
│                                 │   │                                 │
│  Richiede un account            │   │  Nessuna configurazione         │
│  Cloudflare con bucket R2.      │   │  aggiuntiva richiesta.          │
│                                 │   │                                 │
│         [ Configura → ]         │   │         [ Inizia → ]            │
└─────────────────────────────────┘   └─────────────────────────────────┘
```

- **"Inizia →"** (locale): salta al mode=LOCAL, carica `~/.ssh/config` se esiste, mostra le card. Nessun file `r2.json` viene creato. L'ingranaggio in navbar resta disponibile per configurare R2 in futuro.
- **"Configura →"** (cloud): passa allo Step 2.

### Step 2 — Credenziali R2

Form inline (stesso contenuto di `sshpad-r2-settings.js`):
- Endpoint URL
- Access Key ID
- Secret Access Key
- Bucket
- Object Key (default: `sshpad-config.spd`)
- Pulsante "Testa Connessione" con feedback inline
- Pulsante "Avanti →" (abilitato solo dopo test OK o skip manuale)
- Pulsante "← Indietro" per tornare allo Step 1

### Step 3 — Password master

Due scenari:
- **Primo setup cloud** (oggetto non esiste su R2): chiede di **creare** una password master con campo di conferma. Crea un blob vuoto crittografato e lo carica su R2.
- **Setup su nuova macchina** (oggetto già presente su R2): chiede di **inserire** la password master esistente. Scarica e decripta per verificare.

```
┌─────────────────────────────────────────┐
│  🔐 Password Master                    │
│                                         │
│  [Crea/Inserisci] la password per       │
│  [proteggere/sbloccare] le connessioni. │
│                                         │
│  Password:     [________________]       │
│  Conferma:     [________________]  ← solo se prima volta
│                                         │
│  ⚠️ Questa password non è recuperabile. │
│                                         │
│     [← Indietro]         [Completa →]   │
└─────────────────────────────────────────┘
```

- Dopo completamento: salva `r2.json`, passa a mode=CLOUD, mostra le card.

### Backend: rilevamento primo avvio

`cm_create()` determina la modalità iniziale:

```
r2.json esiste?
  ├─ SÌ → mode = LOCKED (come prima)
  └─ NO → mode = FIRST_RUN
```

Nuovo endpoint:

| Metodo | Path | Body | Funzione |
|--------|------|------|----------|
| POST | `/api/config/setup` | `{"mode":"cloud","r2":{...},"password":"..."}` oppure `{"mode":"local"}` | Completa il wizard. Se cloud: salva r2.json, crea/decripta blob, passa a CLOUD. Se local: passa a LOCAL. |

### UI: `ui/components/sshpad-wizard.js`

Componente multi-step con stato interno `step` (1, 2, 3).

**Properties**:
- `step` (Number, state) — step corrente
- `r2Config` (Object, state) — credenziali inserite
- `testResult` (String, state) — esito test connessione
- `isNewSetup` (Boolean, state) — true se oggetto non esiste su R2 (prima volta)
- `loading` (Boolean, state)
- `error` (String, state)

**Eventi**:
- `wizard-complete` con `detail: { mode: "cloud" | "local" }` — sshpad-app nasconde wizard e procede

**Registrazione**: aggiungere in `ui/main.js`.

### Flusso startup aggiornato

```
cm_create()
  ├─ r2.json esiste → mode=LOCKED → UI mostra sshpad-unlock
  └─ r2.json non esiste → mode=FIRST_RUN → UI mostra sshpad-wizard
       ├─ Utente sceglie "Solo Locale" → POST /api/config/setup {mode:"local"} → mode=LOCAL
       └─ Utente sceglie "Cloud" → compila R2 + password
            → POST /api/config/setup {mode:"cloud", r2:{...}, password:"..."}
            → Backend: salva r2.json, GET R2 oggetto
                 ├─ 404 → crea blob vuoto, PUT su R2 → mode=CLOUD (array vuoto)
                 └─ 200 → decripta con password → mode=CLOUD (host caricati)
```

Agli avvii successivi il wizard non appare più (perché `r2.json` esiste oppure è già stato scelto LOCAL, nel qual caso `cm_create` vede che non c'è `r2.json` e... serve un flag).

### Persistenza della scelta "Solo Locale"

Se l'utente sceglie "Solo Locale" al wizard, serve un modo per non ri-mostrare il wizard al prossimo avvio. Due opzioni:

**Opzione scelta**: creare `~/.config/sshpad/settings.json` con:
```json
{ "mode": "local" }
```

`cm_create()` diventa:
```
settings.json esiste?
  ├─ SÌ, mode="local" → r2.json ignorato → mode=LOCAL
  ├─ SÌ, mode="cloud" → r2.json deve esistere → mode=LOCKED
  └─ NO → mode=FIRST_RUN (mostra wizard)
```

Questo permette anche di tornare al wizard in futuro eliminando `settings.json` o tramite un pulsante "Riconfigura" nelle impostazioni.

---

## Nuova dipendenza: libcurl

S3 richiede HTTPS con firma AWS Signature V4. Implementare un client HTTP/TLS da zero in C è impraticabile. libcurl è universale (`apt install libcurl4-openssl-dev` / `brew install curl`).

---

## Formato file crittografato (`.spd`)

```
Offset  Bytes  Campo
0       4      Magic: "SPD\x01"
4       4      Iterazioni PBKDF2 (uint32 big-endian, default 600000)
8       32     Salt (random)
40      12     IV/Nonce (random)
52      N      Ciphertext (AES-256-GCM)
52+N    16     GCM Authentication Tag
```

- Derivazione chiave: `PBKDF2-HMAC-SHA256(password, salt, iterations) → 32 byte`
- Il plaintext è un array JSON UTF-8 (stesso formato di `/api/hosts`)

---

## File nuovi (backend C)

### `src/crypto_store.h` / `src/crypto_store.c`

Crittografia/decrittografia del blob SPD usando OpenSSL (già linkato).

```c
// Cripta JSON plaintext → blob SPD (malloc'd, caller free)
unsigned char *cs_encrypt(const char *plaintext, const char *password, size_t *out_len);

// Decripta blob SPD → JSON string (malloc'd, caller free). NULL = password errata
char *cs_decrypt(const unsigned char *blob, size_t blob_len, const char *password);
```

Implementazione:
- `cs_encrypt`: `RAND_bytes()` per salt e IV, `PKCS5_PBKDF2_HMAC()` con SHA256, `EVP_aes_256_gcm()`, estrai tag con `EVP_CTRL_GCM_GET_TAG`
- `cs_decrypt`: valida magic, estrai salt/IV/tag, deriva chiave, imposta tag con `EVP_CTRL_GCM_SET_TAG`, decripta. Se `EVP_DecryptFinal_ex` fallisce → password errata

### `src/r2_client.h` / `src/r2_client.c`

Client S3/R2 minimale con libcurl + firma AWS Signature V4.

```c
typedef struct {
    char endpoint[512];         // "https://<account>.r2.cloudflarestorage.com"
    char access_key_id[128];
    char secret_access_key[256];
    char bucket[128];
    char object_key[256];       // default: "sshpad-config.spd"
} r2_config_t;

int r2_config_load(r2_config_t *cfg);                    // da ~/.config/sshpad/r2.json
int r2_config_save(const r2_config_t *cfg);
unsigned char *r2_get_object(const r2_config_t *cfg, size_t *out_len, int *http_status);
int r2_put_object(const r2_config_t *cfg, const unsigned char *data, size_t len);
int r2_test_connection(const r2_config_t *cfg);           // HEAD bucket
```

Firma AWS Sig V4:
- Region = `"auto"` per R2
- Signing key chain: `HMAC-SHA256(HMAC-SHA256(HMAC-SHA256(HMAC-SHA256("AWS4"+secret, date), "auto"), "s3"), "aws4_request")`
- Header: `Authorization`, `x-amz-date`, `x-amz-content-sha256`, `Host`
- OpenSSL `HMAC()` e `SHA256()` per le firme

### `src/config_manager.h` / `src/config_manager.c`

Orchestratore centrale: possiede l'array hosts in memoria, la password, e coordina R2.

```c
typedef enum { CM_MODE_FIRST_RUN, CM_MODE_LOCKED, CM_MODE_CLOUD, CM_MODE_LOCAL } cm_mode_t;
typedef struct config_manager config_manager_t;

config_manager_t *cm_create(sse_broadcaster_t *sse);
int cm_unlock(config_manager_t *cm, const char *password);  // -1=bad pw, -2=network
cm_mode_t cm_get_mode(config_manager_t *cm);
const ssh_host_t *cm_get_hosts(config_manager_t *cm, int *out_count);
int cm_save_host(config_manager_t *cm, const ssh_host_t *host);  // create/update
int cm_delete_host(config_manager_t *cm, const char *name);
int cm_change_password(config_manager_t *cm, const char *old_pw, const char *new_pw);
void cm_get_r2_config(config_manager_t *cm, r2_config_t *out);
int cm_set_r2_config(config_manager_t *cm, const r2_config_t *cfg);
int cm_use_local(config_manager_t *cm);   // bypass R2 per questa sessione, carica ~/.ssh/config
void cm_free(config_manager_t *cm);
```

Struct interno:
```c
struct config_manager {
    pthread_rwlock_t  lock;       // protegge hosts/num_hosts
    ssh_host_t       *hosts;
    int               num_hosts;
    cm_mode_t         mode;
    r2_config_t       r2_cfg;
    int               r2_configured;
    char              password[256];
    sse_broadcaster_t *sse;
};
```

- `cm_create()`: carica `r2.json`. Se esiste → mode=LOCKED. Se no → `parse_ssh_config()` → mode=LOCAL.
- `cm_unlock()`: `r2_get_object()`. HTTP 404 → prima configurazione (crea vuoto, cripta, upload). HTTP 200 → decripta. Fallimento → errore.
- `cm_save_host()` / `cm_delete_host()`: write-lock → modifica array → write-unlock → sync → broadcast SSE `config_changed`
  - In mode=CLOUD: `cm_sync_to_r2()` (serializza JSON → cripta → upload)
  - In mode=LOCAL: `ssh_hosts_write_config()` (scrive `~/.ssh/config` con backup `.bak`)
- Thread safety: `pthread_rwlock_t` — letture concorrenti OK, scritture esclusive

---

## Modifiche a file C esistenti

### `src/config_parser.h` / `src/config_parser.c`

Aggiungere serializzazione/deserializzazione JSON:

```c
ssh_host_t *ssh_hosts_from_json(const char *json_str, int *out_count);
char *ssh_hosts_to_json(const ssh_host_t *hosts, int count);
```

I nomi dei campi JSON sono quelli già usati in `handle_get_hosts()`: `name`, `hostname`, `user`, `port`, `identityFile`, `proxyJump`, `localForward` (array di `{bindAddr, bindPort, remoteHost, remotePort}`), `remoteForward`, `dynamicForward`.

Aggiungere anche la scrittura del file `~/.ssh/config`:

```c
// Scrive l'array di host come ~/.ssh/config (sovrascrive il file)
int ssh_hosts_write_config(const ssh_host_t *hosts, int count, const char *path);
```

Genera il file nel formato standard SSH config:
```
Host alias
    HostName hostname
    User user
    Port 22
    IdentityFile ~/.ssh/id_rsa
    ProxyJump bastion
    LocalForward bindAddr:bindPort remoteHost:remotePort
    RemoteForward bindAddr:bindPort remoteHost:remotePort
    DynamicForward bindAddr:bindPort
```

Prima di sovrascrivere, crea un backup `~/.ssh/config.bak`.

### `src/app_context.h`

```c
typedef struct app_context {
    int                port;
    struct MHD_Daemon *httpd;
    sse_broadcaster_t *sse;
    process_manager_t *pm;
    config_manager_t  *cm;              // NUOVO: sostituisce hosts/num_hosts
    char               askpass_path[512];
    local_proxy_t     *proxy;
} app_context_t;
```

Rimuovere `ssh_host_t *hosts` e `int num_hosts`. Tutti i riferimenti usano `cm_get_hosts(ctx->cm, &count)`.

### `src/main.c`

```c
// Vecchio:
ctx.hosts = parse_ssh_config(NULL, &ctx.num_hosts);

// Nuovo:
ctx.cm = cm_create(ctx.sse);
// curl_global_init(CURL_GLOBAL_DEFAULT) all'inizio di main()
// curl_global_cleanup() in cleanup()
```

Aggiornare `cleanup()`: `cm_free(ctx->cm)` invece di `ssh_hosts_free()`.
Aggiornare `lp_create()`: passare hosts da `cm_get_hosts()`.

### `src/http_server.c`

**Aggiornare handler esistenti** per usare `cm_get_hosts(ctx->cm, &count)`:
- `handle_get_hosts()` — itera su `cm_get_hosts()`
- `handle_tunnel_start()` — cerca host via `cm_get_hosts()`
- `handle_internal_askpass()` — idem

**7 nuovi endpoint**:

| Metodo | Path | Body/Params | Funzione |
|--------|------|-------------|----------|
| GET | `/api/config/status` | — | `{"mode":"first_run\|locked\|cloud\|local","r2Configured":bool}` |
| POST | `/api/config/setup` | `{"mode":"cloud","r2":{...},"password":"..."}` o `{"mode":"local"}` | Completa wizard primo avvio |
| POST | `/api/config/unlock` | `{"password":"..."}` | Scarica, decripta, carica. `{"ok":true}` o errore |
| POST | `/api/host/save` | JSON host completo | Crea/aggiorna host, sync R2 |
| POST | `/api/host/delete` | `{"name":"alias"}` | Elimina host, sync R2 |
| POST | `/api/config/use-local` | — | Bypassa R2 per questa sessione, carica ~/.ssh/config |
| POST | `/api/config/change-password` | `{"oldPassword":"...","newPassword":"..."}` | Re-cripta e upload |
| GET | `/api/config/r2-settings` | — | Config R2 (secret mascherata) |
| POST | `/api/config/r2-settings` | JSON r2_config | Salva config R2. Se mode=CLOUD, ri-sincronizza con le nuove credenziali |
| POST | `/api/config/r2-test` | JSON r2_config (opzionale) | Testa connessione R2. Se body vuoto usa credenziali salvate, altrimenti testa quelle fornite. Ritorna `{"ok":true}` o `{"error":"..."}` |

### `CMakeLists.txt`

```cmake
pkg_check_modules(CURL REQUIRED libcurl)
# Aggiungere ai target: crypto_store.c, r2_client.c, config_manager.c
# Aggiungere: ${CURL_INCLUDE_DIRS}, ${CURL_CFLAGS_OTHER}, ${CURL_LIBRARIES}
```

---

## File nuovi (UI)

### `ui/components/sshpad-unlock.js` — Dialog sblocco all'avvio

Modale Bootstrap non-dismissibile (`data-bs-backdrop="static"`).

- Input password + pulsante "Sblocca" (con spinner durante caricamento)
- Messaggio errore inline se password errata
- Pulsante "Usa configurazione locale" come fallback → per la sessione corrente bypassa R2 e carica `~/.ssh/config` (mode=LOCAL, editabile). Al prossimo avvio verrà richiesta nuovamente la password.
- Link "Impostazioni R2" per aprire settings
- Evento `unlocked` al successo → sshpad-app prosegue con `_fetchHosts()`
- Evento `use-local` → backend chiama `cm_use_local()` che carica `parse_ssh_config()` e imposta mode=LOCAL senza disabilitare R2 (la configurazione R2 resta salvata, viene solo saltata per questa sessione)
- In mode=LOCAL le modifiche vengono scritte direttamente su `~/.ssh/config`

### `ui/components/sshpad-host-editor.js` — Editor connessione

Modale Bootstrap grande (`modal-lg`) per creare/modificare host.

Campi form:
- **Host alias** (name) — text, required, readonly in edit mode
- **HostName** — text
- **User** — text
- **Port** — number (default 22)
- **IdentityFile** — text
- **ProxyJump** — text
- **Local Forwards** — lista dinamica con +/- (bindAddr, bindPort, remoteHost, remotePort per riga)
- **Remote Forwards** — idem
- **Dynamic Forwards** — lista dinamica (bindAddr, bindPort per riga)

Metodi pubblici:
- `show(host)` — apre in modalità modifica, campi precompilati
- `showNew()` — apre in modalità creazione, campi vuoti

Evento: `host-save` con `detail: { host: {...} }`

### `ui/components/sshpad-r2-settings.js` — Impostazioni R2

Modale accessibile **in qualsiasi momento** tramite l'icona ingranaggio in navbar (visibile in tutte le modalità: CLOUD, LOCAL, LOCKED).

Form per: endpoint, accessKeyId, secretAccessKey (password field), bucket, objectKey.

- Pulsante **"Testa Connessione"** → `POST /api/config/r2-test` con le credenziali attualmente nel form (non quelle salvate) → feedback inline verde/rosso. Permette di verificare credenziali nuove prima di salvarle.
- Pulsante **"Salva"** → `POST /api/config/r2-settings`
- Carica valori attuali da `GET /api/config/r2-settings`
- Secret mascherata dal server (primi 4 char + `****`), se utente non modifica → mantiene
- Pulsante **"Riconfigura"** (solo se `settings.json` esiste) → elimina `settings.json`, al riavvio riappare wizard

**Cambio credenziali a caldo (mode=CLOUD)**:
Quando l'utente modifica e salva le credenziali R2 mentre è in mode=CLOUD, il backend:
1. Salva le nuove credenziali in `r2.json`
2. Ri-cripta la config attuale in memoria
3. Esegue `r2_put_object()` con le nuove credenziali per caricare il blob sul nuovo bucket/endpoint
4. Se il PUT fallisce → rollback a credenziali precedenti, toast errore
5. Se il PUT riesce → le nuove credenziali diventano attive

**Cambio credenziali da LOCAL a CLOUD**:
Se l'utente è in mode=LOCAL e configura R2 per la prima volta via impostazioni:
1. Salva credenziali in `r2.json`
2. Aggiorna `settings.json` con `mode:"cloud"`
3. Mostra dialog password master (crea nuova o inserisci esistente, come Step 3 del wizard)
4. Dopo sblocco → mode passa a CLOUD, host attuali sincronizzati su R2

---

## Modifiche UI esistenti

### `ui/components/sshpad-card.js`

Nuova property: `editable` (Boolean).

Aggiungere nella card header (solo se `editable`):
- Pulsante matita (edit) → evento `host-edit`
- Pulsante cestino (delete) → evento `host-delete`

### `ui/components/sshpad-app.js`

1. **Nuove properties**: `configMode` (first_run/locked/cloud/local)
2. **Startup flow rivisto**:
   ```
   _init() → GET /api/config/status
     → mode=first_run? → mostra sshpad-wizard, attendi wizard-complete
     → mode=locked?    → mostra sshpad-unlock, attendi sblocco
     → mode=local?     → _fetchHosts() + _connectSSE()
     → mode=cloud?     → _fetchHosts() + _connectSSE()
   ```
3. **Navbar**: pulsante "+" (nuovo host, solo cloud), icona ingranaggio (settings R2), badge modo (Cloud/Locale)
4. **Event handler nuovi**: `_onHostEdit`, `_onHostDelete` (con conferma), `_onHostSave`, `_onUnlocked`, `_onUseLocal`
5. **Render**: includere `<sshpad-wizard>`, `<sshpad-unlock>`, `<sshpad-host-editor>`, `<sshpad-r2-settings>`
6. Passare `editable=${true}` a ogni `<sshpad-card>` (editing abilitato in tutte le modalità: CLOUD scrive su R2, LOCAL scrive su ~/.ssh/config)

### `ui/main.js`

Aggiungere import dei 3 nuovi componenti.

---

## Flussi principali

### Primo avvio in assoluto
```
main.c → cm_create() → nessun settings.json → mode=FIRST_RUN
UI load → GET /api/config/status → {mode:"first_run"}
UI → mostra sshpad-wizard
  Scelta "Cloud" → Step 2 (R2 creds) → Step 3 (password)
    → POST /api/config/setup {mode:"cloud", r2:{...}, password:"..."}
    → Backend: salva settings.json + r2.json, GET/PUT R2, mode=CLOUD
  Scelta "Locale" →
    → POST /api/config/setup {mode:"local"}
    → Backend: salva settings.json, parse_ssh_config(), mode=LOCAL
UI → wizard-complete → _fetchHosts() + _connectSSE()
```

### Startup (R2 configurato)
```
main.c → cm_create() → settings.json mode=cloud + r2.json → mode=LOCKED
UI load → GET /api/config/status → {mode:"locked"}
UI → mostra sshpad-unlock
Utente inserisce password → POST /api/config/unlock
Backend → r2_get_object() → cs_decrypt() → carica hosts → mode=CLOUD
UI → nasconde unlock → _fetchHosts() → mostra cards con edit/delete
```

### Prima configurazione (R2 configurato, nessun file remoto)
```
POST /api/config/unlock → r2_get_object() → HTTP 404
Backend → crea array vuoto → cs_encrypt() → r2_put_object() → mode=CLOUD
UI → "Nessun host. Clicca + per aggiungere."
```

### Modifica host
```
Click matita → sshpad-host-editor.show(host) → utente modifica → Salva
POST /api/host/save → cm_save_host() → aggiorna array → cs_encrypt() → r2_put_object()
SSE config_changed → UI aggiorna cards
```

### Avvio in modalità locale (dopo wizard o settings.json mode=local)
```
cm_create() → settings.json mode=local → parse_ssh_config() → mode=LOCAL
UI → GET /api/config/status → {mode:"local"}
UI → _fetchHosts() → cards con edit/delete → badge "Locale"
Modifiche → POST /api/host/save → cm_save_host() → ssh_hosts_write_config() → ~/.ssh/config aggiornato
```

---

## Sequenza implementazione

### Fase 1 — Backend core (testabile con curl)
1. `src/crypto_store.c/.h` — crittografia/decrittografia
2. `src/r2_client.c/.h` — client S3/R2 con AWS Sig V4
3. `src/config_parser.c` — aggiungere `ssh_hosts_from_json()` / `ssh_hosts_to_json()`
4. `src/config_manager.c/.h` — orchestratore
5. `CMakeLists.txt` — libcurl + nuovi sorgenti

### Fase 2 — Integrazione API
6. `src/app_context.h` — sostituire hosts/num_hosts con cm
7. `src/http_server.c` — 7 nuovi endpoint + aggiornare quelli esistenti
8. `src/main.c` — usare cm_create(), curl_global_init()

### Fase 3 — UI
9. `ui/components/sshpad-wizard.js` — wizard primo avvio (3 step)
10. `ui/components/sshpad-unlock.js`
11. `ui/components/sshpad-r2-settings.js`
12. `ui/components/sshpad-host-editor.js`
13. `ui/components/sshpad-card.js` — edit/delete buttons
14. `ui/components/sshpad-app.js` — nuovo startup flow + event handlers
15. `ui/main.js` — registrazione nuovi componenti

### Fase 4 — Polish
16. Gestione errori (R2 timeout, sync falliti → toast)
17. Aggiornamento local proxy quando config cambia in cloud mode

---

## Verifica

### T1 — Crittografia (unit test, `test_crypto.c`)

| # | Test | Input | Esito atteso |
|---|------|-------|--------------|
| 1.1 | Round-trip base | Stringa JSON di esempio, password "test123" | `cs_encrypt()` → blob → `cs_decrypt()` → JSON identico all'originale |
| 1.2 | Password errata | Blob crittato con "abc", decripta con "xyz" | `cs_decrypt()` ritorna NULL |
| 1.3 | Blob corrotto | Alterare 1 byte nel ciphertext | `cs_decrypt()` ritorna NULL (GCM tag mismatch) |
| 1.4 | Magic errato | Blob con magic "XXX\x00" | `cs_decrypt()` ritorna NULL immediatamente |
| 1.5 | Blob troppo corto | Buffer di 10 byte | `cs_decrypt()` ritorna NULL senza crash |
| 1.6 | Plaintext vuoto | `"[]"` (array JSON vuoto) | Round-trip OK, decripta a `"[]"` |
| 1.7 | Plaintext grande | JSON con 100 host (~200KB) | Round-trip OK, nessun troncamento |
| 1.8 | Password con caratteri speciali | `"p@$$w0rd!è€"` (UTF-8, simboli) | Round-trip OK |
| 1.9 | Salt/IV unicità | Crittare lo stesso plaintext 2 volte con stessa password | Blob diversi (salt e IV random diversi) |

### T2 — Client R2 (richiede bucket R2 reale)

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 2.1 | Test connessione OK | `r2_test_connection()` con credenziali valide | Ritorna 0 |
| 2.2 | Test connessione KO | `r2_test_connection()` con access_key errata | Ritorna errore (!= 0) |
| 2.3 | GET oggetto inesistente | `r2_get_object()` su chiave mai creata | `http_status` = 404, ritorna NULL |
| 2.4 | PUT + GET round-trip | `r2_put_object()` con dati, poi `r2_get_object()` | Dati identici, `http_status` = 200 |
| 2.5 | PUT sovrascrittura | `r2_put_object()` due volte con dati diversi, poi GET | Ritorna i dati della seconda PUT |
| 2.6 | Endpoint errato | URL endpoint inesistente | `r2_get_object()` ritorna NULL, nessun crash |
| 2.7 | Timeout rete | Simulare con endpoint che non risponde (es. `http://192.0.2.1:1`) | Ritorna errore entro timeout ragionevole (<30s) |
| 2.8 | Caricamento config | `r2_config_load()` con `~/.config/sshpad/r2.json` valido | Tutti i campi popolati correttamente |
| 2.9 | Config assente | `r2_config_load()` senza file `r2.json` | Ritorna errore, struct non modificata |
| 2.10 | Salvataggio config | `r2_config_save()` → `r2_config_load()` | Round-trip identico |

### T3 — Serializzazione JSON ↔ ssh_host_t

| # | Test | Input | Esito atteso |
|---|------|-------|--------------|
| 3.1 | Serializzazione completa | Host con tutti i campi compilati (name, hostname, user, port, identity, proxy, 3 local fwd, 2 remote fwd, 1 dynamic fwd) | JSON valido con tutti i campi |
| 3.2 | Deserializzazione completa | JSON del test 3.1 | `ssh_host_t` con tutti i campi identici |
| 3.3 | Round-trip array | Array di 5 host diversi → JSON → parse | Array identico campo per campo |
| 3.4 | Host minimale | Solo `name` e `hostname`, resto default | JSON con campi vuoti/default, deserializzazione corretta |
| 3.5 | Array vuoto | `"[]"` | `out_count` = 0, puntatore non NULL |
| 3.6 | JSON malformato | `"{broken"` | `ssh_hosts_from_json()` ritorna NULL |
| 3.7 | Caratteri speciali nei valori | Hostname con `-`, `.`, user con `_` | Round-trip OK |

### T4 — Scrittura ~/.ssh/config

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 4.1 | Scrittura base | `ssh_hosts_write_config()` con 3 host | File `~/.ssh/config` valido, parsabile da `parse_ssh_config()` con risultato identico |
| 4.2 | Round-trip completo | `parse_ssh_config()` → `ssh_hosts_write_config()` → `parse_ssh_config()` | Secondo parse identico al primo |
| 4.3 | Backup creato | Scrivere config quando esiste già | `~/.ssh/config.bak` creato con contenuto precedente |
| 4.4 | Forward nel formato corretto | Host con LocalForward, RemoteForward, DynamicForward | Output: `LocalForward bindAddr:bindPort remoteHost:remotePort` (formato standard SSH) |
| 4.5 | Permessi file | Dopo scrittura | File con permessi 0600 (rw-------) |
| 4.6 | Directory .ssh assente | Rimuovere `~/.ssh/` | Crea la directory con permessi 0700, poi scrive il file |

### T5 — Config Manager (integrazione backend)

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 5.1 | Avvio senza R2 | `r2.json` assente | `cm_get_mode()` = LOCAL, host caricati da `~/.ssh/config` |
| 5.2 | Avvio con R2 | `r2.json` presente | `cm_get_mode()` = LOCKED, `cm_get_hosts()` ritorna 0 host |
| 5.3 | Unlock con password corretta | `cm_unlock("password")` con blob R2 valido | mode=CLOUD, host caricati |
| 5.4 | Unlock con password errata | `cm_unlock("wrong")` | Ritorna -1, mode resta LOCKED |
| 5.5 | Unlock con R2 irraggiungibile | `cm_unlock()` con endpoint errato | Ritorna -2, mode resta LOCKED |
| 5.6 | Prima configurazione | `cm_unlock()` con R2 raggiungibile ma oggetto 404 | mode=CLOUD, array vuoto, blob caricato su R2 |
| 5.7 | Bypass locale | `cm_use_local()` da mode=LOCKED | mode=LOCAL, host da `~/.ssh/config`, R2 config conservata |
| 5.8 | Save host (CLOUD) | `cm_save_host()` con nuovo host | Host aggiunto in memoria, blob aggiornato su R2 |
| 5.9 | Save host (LOCAL) | `cm_save_host()` in mode=LOCAL | Host aggiunto, `~/.ssh/config` riscritto |
| 5.10 | Update host | `cm_save_host()` con host esistente (stesso name) | Host sostituito, non duplicato |
| 5.11 | Delete host | `cm_delete_host("alias")` | Host rimosso, sync eseguita |
| 5.12 | Delete host inesistente | `cm_delete_host("nonexiste")` | Ritorna errore, nessun side effect |
| 5.13 | Cambio password | `cm_change_password("old", "new")` | Blob ri-crittato con nuova password, vecchia non funziona più |
| 5.14 | Cambio password errata | `cm_change_password("wrong", "new")` | Ritorna errore, blob invariato |
| 5.15 | Concorrenza lettura | 10 thread che chiamano `cm_get_hosts()` simultaneamente | Nessun crash, dati consistenti |
| 5.16 | Concorrenza scrittura | 2 thread che chiamano `cm_save_host()` simultaneamente | Entrambi gli host salvati, nessun data race |

### T6 — API HTTP (testabile con curl)

| # | Test | Comando | Esito atteso |
|---|------|---------|--------------|
| 6.1 | Status iniziale (R2) | `curl /api/config/status` | `{"mode":"locked","r2Configured":true}` |
| 6.2 | Status iniziale (no R2) | `curl /api/config/status` | `{"mode":"local","r2Configured":false}` |
| 6.3 | Unlock OK | `curl -X POST /api/config/unlock -d '{"password":"test"}'` | `{"ok":true}`, status diventa `cloud` |
| 6.4 | Unlock password errata | `curl -X POST /api/config/unlock -d '{"password":"wrong"}'` | `{"error":"bad_password"}` |
| 6.5 | Unlock rete KO | Endpoint R2 irraggiungibile | `{"error":"network_error"}` |
| 6.6 | Use local | `curl -X POST /api/config/use-local` | `{"ok":true}`, status diventa `local` |
| 6.7 | Host save (nuovo) | `curl -X POST /api/host/save -d '{"name":"test",...}'` | `{"ok":true}`, host visibile in `/api/hosts` |
| 6.8 | Host save (update) | Stessa chiamata con dati modificati | Host aggiornato, non duplicato |
| 6.9 | Host save senza name | Body JSON senza campo `name` | `{"error":"missing field: name"}` |
| 6.10 | Host delete | `curl -X POST /api/host/delete -d '{"name":"test"}'` | `{"ok":true}`, host rimosso da `/api/hosts` |
| 6.11 | Host delete inesistente | `curl -X POST /api/host/delete -d '{"name":"nope"}'` | `{"error":"host not found"}` |
| 6.12 | Change password | `curl -X POST /api/config/change-password -d '{"oldPassword":"a","newPassword":"b"}'` | `{"ok":true}` |
| 6.13 | R2 settings GET | `curl /api/config/r2-settings` | JSON con secret mascherata (`ABCD****`) |
| 6.14 | R2 settings POST | `curl -X POST /api/config/r2-settings -d '{...}'` | `{"ok":true}`, file `r2.json` aggiornato |
| 6.15 | R2 settings preserva secret | POST con `secretAccessKey: "ABCD****"` | Secret precedente mantenuta nel file |
| 6.16 | R2 test con credenziali nel body | `curl -X POST /api/config/r2-test -d '{"endpoint":"...","accessKeyId":"...","secretAccessKey":"...","bucket":"..."}'` | `{"ok":true}` se raggiungibile |
| 6.17 | R2 test con credenziali errate | Body con access_key errata | `{"error":"AccessDenied"}` o simile |
| 6.18 | R2 test senza body | `curl -X POST /api/config/r2-test` (body vuoto) | Usa credenziali salvate in `r2.json`, testa quelle |
| 6.19 | R2 test senza r2.json | Body vuoto e nessun `r2.json` | `{"error":"no R2 configuration"}` |
| 6.20 | SSE config_changed | Dopo host save/delete | Evento SSE `config_changed` ricevuto dai client |
| 6.21 | Hosts dopo unlock | `curl /api/hosts` dopo unlock | JSON array con host decriptati |
| 6.22 | Hosts prima di unlock | `curl /api/hosts` con mode=locked | Array vuoto `[]` |

### T7 — UI: Wizard primo avvio

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 7.1 | Wizard appare al primo avvio | Avviare senza `settings.json` né `r2.json` | Wizard a schermo intero con due card (Cloud / Locale) |
| 7.2 | Scelta locale | Click "Inizia →" su card Locale | Wizard scompare, badge "Locale", `settings.json` creato con `mode:"local"` |
| 7.3 | Scelta locale con ssh config | `~/.ssh/config` con 3 host, scegliere Locale | 3 card visibili dopo il wizard |
| 7.4 | Scelta cloud → Step 2 | Click "Configura →" su card Cloud | Form credenziali R2 visibile |
| 7.5 | Step 2 — test connessione OK | Inserire credenziali valide, click "Testa Connessione" | Feedback verde, pulsante "Avanti" abilitato |
| 7.6 | Step 2 — test connessione KO | Inserire credenziali errate, click "Testa" | Feedback rosso con dettaglio errore |
| 7.7 | Step 2 — indietro | Click "← Indietro" | Torna a Step 1 (scelta modalità) |
| 7.8 | Step 3 — primo setup cloud | Oggetto non esiste su R2 | Mostra "Crea una password master" con campo conferma |
| 7.9 | Step 3 — setup su nuova macchina | Oggetto già presente su R2 | Mostra "Inserisci la password master" senza campo conferma |
| 7.10 | Step 3 — password mismatch | Creare password, conferma diversa | Errore "Le password non corrispondono" |
| 7.11 | Step 3 — completa primo setup | Password + conferma uguali, click "Completa" | Wizard chiude, blob vuoto su R2, mode=CLOUD, messaggio "Clicca + per aggiungere" |
| 7.12 | Step 3 — completa nuova macchina | Password corretta, click "Completa" | Wizard chiude, host dal cloud visibili |
| 7.13 | Step 3 — password errata (nuova macchina) | Password sbagliata | Errore inline "Password errata", input svuotato |
| 7.14 | Step 3 — indietro | Click "← Indietro" | Torna a Step 2 con credenziali R2 preservate |
| 7.15 | Wizard non riappare | Riavviare dopo aver completato il wizard | Nessun wizard, va diretto a unlock (cloud) o cards (locale) |
| 7.16 | Riconfigura | Pulsante "Riconfigura" nelle impostazioni → riavvio | Wizard riappare (settings.json eliminato) |

### T8 — UI: Dialog sblocco

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 8.1 | Modale appare all'avvio | Avviare con R2 configurato (dopo wizard completato) | Modale non-dismissibile visibile, sfondo bloccato |
| 8.2 | Password corretta | Inserire password, click "Sblocca" | Spinner → modale si chiude → cards visibili |
| 8.3 | Password errata | Inserire password sbagliata | Messaggio errore inline, input svuotato e rifocusato |
| 8.4 | Retry dopo errore | Inserire password corretta dopo errore | Sblocco riuscito normalmente |
| 8.5 | Enter per submit | Premere Enter nel campo password | Equivalente a click su "Sblocca" |
| 8.6 | Usa locale | Click "Usa configurazione locale" | Modale si chiude, badge "Locale", cards da ~/.ssh/config |
| 8.7 | Apri R2 settings | Click "Impostazioni R2" dal dialog | Si apre il dialog R2 settings |
| 8.8 | Escape non chiude | Premere Escape | Modale resta aperta (backdrop static) |

### T9 — UI: Editor host

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 9.1 | Apri in modifica | Click matita su card | Modale con campi precompilati, campo name readonly |
| 9.2 | Apri in creazione | Click "+" in navbar | Modale con campi vuoti, campo name editabile |
| 9.3 | Salva modifica | Modificare hostname, click Salva | Card aggiornata, toast "Host salvato" |
| 9.4 | Salva nuovo host | Compilare form, click Salva | Nuova card appare nella griglia |
| 9.5 | Validazione name vuoto | Lasciare name vuoto, click Salva | Errore validazione inline, form non inviato |
| 9.6 | Validazione name duplicato | Creare host con name già esistente | Errore "Host già esistente" |
| 9.7 | Aggiungi forward | Click "+" nella sezione Local Forwards | Nuova riga con 4 input (bindAddr, bindPort, remoteHost, remotePort) |
| 9.8 | Rimuovi forward | Click cestino su riga forward | Riga rimossa |
| 9.9 | Salva con forward | Aggiungere 2 local forward + 1 dynamic | Forwards visibili nella card dopo salvataggio |
| 9.10 | Annulla | Modificare campi, click Annulla | Modale chiuso, nessuna modifica applicata |
| 9.11 | Port default | Non compilare porta | Valore 22 usato come default |

### T10 — UI: Eliminazione host

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 10.1 | Conferma eliminazione | Click cestino → "Sei sicuro?" → Conferma | Card rimossa, toast "Host eliminato" |
| 10.2 | Annulla eliminazione | Click cestino → "Sei sicuro?" → Annulla | Nessuna modifica |
| 10.3 | Eliminazione con tunnel attivo | Eliminare host con tunnel in stato "active" | Tunnel fermato prima dell'eliminazione, poi host rimosso |

### T11 — UI: Impostazioni R2

| # | Test | Azione | Esito atteso |
|---|------|--------|--------------|
| 11.1 | Accessibile in CLOUD | Click ingranaggio in navbar mentre mode=CLOUD | Dialog si apre con credenziali correnti |
| 11.2 | Accessibile in LOCAL | Click ingranaggio in navbar mentre mode=LOCAL | Dialog si apre (campi vuoti se mai configurato R2) |
| 11.3 | Accessibile da unlock | Click "Impostazioni R2" dal dialog sblocco | Dialog si apre sopra il dialog unlock |
| 11.4 | Caricamento valori | Aprire dialog con R2 già configurato | Campi precompilati, secret mascherata (primi 4 char + `****`) |
| 11.5 | Testa connessione OK | Inserire credenziali valide nel form, click "Testa Connessione" | Feedback verde "Connessione riuscita" |
| 11.6 | Testa connessione KO | Inserire credenziali errate, click "Testa" | Feedback rosso "Connessione fallita: ..." con dettaglio |
| 11.7 | Testa prima di salvare | Modificare endpoint, testare senza salvare | Test usa credenziali del form (non quelle salvate) |
| 11.8 | Salva nuove credenziali | Modificare endpoint e bucket, click Salva | Modale chiuso, `r2.json` aggiornato |
| 11.9 | Secret non modificata | Non toccare il campo secret, Salva | Secret precedente mantenuta |
| 11.10 | Cambio credenziali a caldo (CLOUD) | In mode=CLOUD: modificare bucket, Salva | Blob ri-caricato sul nuovo bucket, toast "Sincronizzazione riuscita" |
| 11.11 | Cambio credenziali a caldo — fallimento | In mode=CLOUD: inserire endpoint errato, Salva | Toast errore, rollback a credenziali precedenti, config in memoria invariata |
| 11.12 | Da LOCAL a CLOUD | In mode=LOCAL: compilare R2 per la prima volta, Salva | Dialog password master appare, dopo completamento mode=CLOUD |
| 11.13 | Riconfigura | Pulsante "Riconfigura" → conferma | Elimina `settings.json`, al riavvio riappare wizard |

### T12 — Flussi end-to-end

| # | Test | Scenario completo | Verifica |
|---|------|-------------------|----------|
| 12.1 | Wizard → cloud → primo setup | Wizard: Cloud → credenziali R2 → crea password → completa | Cards vuote, blob `.spd` presente su R2, `settings.json` e `r2.json` creati |
| 12.2 | Wizard → locale | Wizard: Locale | Cards da `~/.ssh/config`, `settings.json` con mode=local, nessun `r2.json` |
| 12.3 | Wizard → cloud → seconda macchina | Wizard: Cloud → stesse credenziali → inserisci password esistente | Host dal cloud visibili |
| 12.4 | Creazione host in cloud | Dopo setup cloud: crea 3 host via "+" | 3 cards, blob aggiornato su R2 |
| 12.5 | Seconda macchina vede le modifiche | Macchina B: avvia → sblocca → verifica | Stessi 3 host visibili |
| 12.6 | Modifica cross-machine | Macchina A: modifica host → Macchina B: riavvia → sblocca | Modifica visibile su macchina B |
| 12.7 | Fallback e ritorno | Avvia con R2 → "Usa locale" → verifica host locali → riavvia → sblocca con password | Torna ai dati cloud, host locali non mischiati |
| 12.8 | Cambio password cross-machine | Macchina A: cambia password → Macchina B: tenta vecchia password | Vecchia password rifiutata, nuova funziona |
| 12.9 | Modalità locale pura | Wizard: Locale → crea host → modifica host → elimina host | Tutto persiste in `~/.ssh/config`, backup `.bak` presente |
| 12.10 | SSH tunnel dopo edit | Creare host con LocalForward → attivare tunnel | Tunnel attivo, forward funzionante, porta mappata |
| 12.11 | Terminale dopo edit | Creare host → click ">_" | Terminale si apre con `ssh <alias>` |
| 12.12 | R2 irraggiungibile durante sessione | Sbloccato → modifica host → stacca rete | Toast errore sync, dati in memoria ancora validi, UI funziona |
| 12.13 | Recovery dopo errore sync | Dopo 12.12: riconnetti rete → modifica altro host | Sync riprende, blob R2 aggiornato con tutte le modifiche |
| 12.14 | Riconfigura da locale a cloud | Mode locale → impostazioni → "Riconfigura" → riavvia → wizard → Cloud | Migra a cloud, host importati dal locale |

### T13 — Sicurezza e robustezza

| # | Test | Scenario | Verifica |
|---|------|----------|----------|
| 13.1 | Password non in log | Avviare con verbose, sbloccare | Password non appare in stdout/stderr |
| 13.2 | Secret R2 mascherata | `GET /api/config/r2-settings` | `secretAccessKey` mostra solo primi 4 char + `****` |
| 13.3 | Permessi r2.json | Dopo `r2_config_save()` | File con permessi 0600 |
| 13.4 | Permessi settings.json | Dopo creazione | File con permessi 0600 |
| 13.5 | Permessi ssh config | Dopo `ssh_hosts_write_config()` | File con permessi 0600 |
| 13.6 | Backup ssh config | Modificare host in mode LOCAL | `~/.ssh/config.bak` contiene versione precedente |
| 13.7 | JSON injection | Creare host con `name: "test\"; rm -rf /"` | Nome rifiutato dalla validazione (solo alfanumerici, `-`, `_`, `.`) |
| 13.8 | Blob troppo grande | Simulare risposta R2 da 100MB | Client rifiuta/tronca, nessun OOM |
| 13.9 | Richieste API senza unlock | `POST /api/host/save` con mode=LOCKED | `{"error":"config not unlocked"}` |
| 13.10 | Richieste API durante FIRST_RUN | `POST /api/host/save` con mode=FIRST_RUN | `{"error":"setup not completed"}` |

### Ordine di esecuzione dei test

1. **T1** (crypto) → indipendente, primo da implementare
2. **T3** (serializzazione JSON) → indipendente, parallelizzabile con T1
3. **T4** (scrittura ssh config) → dipende da T3
4. **T2** (client R2) → richiede bucket reale, parallelizzabile con T3/T4
5. **T5** (config manager) → dipende da T1, T2, T3, T4
6. **T6** (API HTTP) → dipende da T5
7. **T7** (wizard) → dipende da T6
8. **T8–T11** (UI: unlock, editor, eliminazione, R2 settings) → dipendono da T6
9. **T12** (end-to-end) → tutti i componenti pronti
10. **T13** (sicurezza) → trasversale, eseguire per ultimo

---

## Implementazione con Swarm di Agenti

Questa sezione definisce come parallelizzare l'implementazione usando agenti Sonnet indipendenti. Ogni agente riceve un ruolo preciso, i file su cui opera, le interfacce da rispettare e il contesto minimo necessario.

### Principi generali

- **Ogni agente opera su file propri**: nessun agente scrive sugli stessi file di un altro. I conflitti si risolvono nell'integrazione.
- **Contratti di interfaccia rigidi**: le signature C (header) e le API REST sono definite in anticipo. Ogni agente li implementa senza modificarli.
- **Nessun agente legge il documento intero**: ogni agente riceve solo la sezione di questo documento rilevante al suo ruolo, più i contratti di interfaccia.
- **Build incrementale**: ogni agente produce codice compilabile in isolamento (eventualmente con stub/mock delle dipendenze).

### Definizione agenti

```
┌──────────────────────────────────────────────────────────────────────┐
│                        WAVE 1 (parallelo)                            │
│                                                                      │
│  Agent-CRYPTO    Agent-R2        Agent-JSON       Agent-SSHWRITE     │
│  crypto_store    r2_client       config_parser    config_parser      │
│  .c/.h           .c/.h           (JSON funcs)     (write func)       │
│                                                                      │
├──────────────────────────────────────────────────────────────────────┤
│                        WAVE 2 (dopo Wave 1)                          │
│                                                                      │
│  Agent-CM                                                            │
│  config_manager.c/.h                                                 │
│  (dipende da CRYPTO, R2, JSON, SSHWRITE)                             │
│                                                                      │
├──────────────────────────────────────────────────────────────────────┤
│                        WAVE 3 (parallelo, dopo Wave 2)               │
│                                                                      │
│  Agent-API       Agent-WIZARD    Agent-UNLOCK     Agent-EDITOR       │
│  http_server.c   sshpad-wizard   sshpad-unlock    sshpad-host-editor │
│  main.c          .js             .js              .js                │
│  app_context.h                                                       │
│  CMakeLists.txt                                                      │
│                                                                      │
│                  Agent-R2SET     Agent-CARD                           │
│                  sshpad-r2-      sshpad-card.js   │
│                  settings.js     (edit/delete)    │
│                                                                      │
├──────────────────────────────────────────────────────────────────────┤
│                        WAVE 4 (dopo Wave 3)                          │
│                                                                      │
│  Agent-INTEGRATE                                                     │
│  sshpad-app.js, main.js (UI), assemblaggio finale                    │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

---

### Agent-CRYPTO — Crittografia SPD

**File da creare**: `src/crypto_store.h`, `src/crypto_store.c`

**Contesto da fornire**:
- Sezione "Formato file crittografato (.spd)" di questo documento
- Sezione `src/crypto_store.h / .c`
- OpenSSL è già linkato nel progetto (vedi CMakeLists.txt)

**Contratto di interfaccia (header)**:
```c
#ifndef CRYPTO_STORE_H
#define CRYPTO_STORE_H
#include <stddef.h>

// Cripta plaintext JSON → blob SPD. Caller deve free() il risultato.
// Ritorna NULL in caso di errore.
unsigned char *cs_encrypt(const char *plaintext, const char *password, size_t *out_len);

// Decripta blob SPD → stringa JSON (null-terminated). Caller deve free().
// Ritorna NULL se password errata o blob invalido.
char *cs_decrypt(const unsigned char *blob, size_t blob_len, const char *password);

#endif
```

**Dettagli implementativi**:
- Magic bytes: `"SPD\x01"` (4 byte)
- PBKDF2 iterazioni: 600000, memorizzate come uint32 big-endian ai byte 4-7
- Salt: 32 byte random (offset 8)
- IV: 12 byte random (offset 40)
- Cipher: AES-256-GCM
- Tag GCM: 16 byte, appeso dopo il ciphertext
- Funzioni OpenSSL: `RAND_bytes()`, `PKCS5_PBKDF2_HMAC()` con `EVP_sha256()`, `EVP_aes_256_gcm()`, `EVP_CTRL_GCM_GET_TAG`/`SET_TAG`
- `cs_decrypt`: validare magic e lunghezza minima (52 + 16 = 68 byte) prima di procedere

**Test di validazione**: T1.1–T1.9 (round-trip, password errata, blob corrotto, magic errato, blob corto, vuoto, grande, charset, unicità salt/IV)

---

### Agent-R2 — Client S3/R2

**File da creare**: `src/r2_client.h`, `src/r2_client.c`

**Contesto da fornire**:
- Sezione `src/r2_client.h / .c` di questo documento
- libcurl sarà aggiunto come dipendenza (Agent-API si occupa del CMakeLists.txt)
- OpenSSL è disponibile per HMAC/SHA256

**Contratto di interfaccia (header)**:
```c
#ifndef R2_CLIENT_H
#define R2_CLIENT_H
#include <stddef.h>

typedef struct {
    char endpoint[512];
    char access_key_id[128];
    char secret_access_key[256];
    char bucket[128];
    char object_key[256];
} r2_config_t;

// Carica config da ~/.config/sshpad/r2.json. Ritorna 0 OK, -1 errore.
int r2_config_load(r2_config_t *cfg);

// Salva config su ~/.config/sshpad/r2.json (permessi 0600). Ritorna 0 OK.
int r2_config_save(const r2_config_t *cfg);

// Scarica oggetto. Caller deve free() il risultato. http_status riceve 200/404/etc.
// Ritorna NULL in caso di errore di rete (http_status = 0) o 404.
unsigned char *r2_get_object(const r2_config_t *cfg, size_t *out_len, int *http_status);

// Carica oggetto. Ritorna 0 OK, -1 errore.
int r2_put_object(const r2_config_t *cfg, const unsigned char *data, size_t len);

// Testa connessione con HEAD bucket. Ritorna 0 OK, -1 errore.
int r2_test_connection(const r2_config_t *cfg);

#endif
```

**Dettagli implementativi**:
- AWS Signature V4 con region = `"auto"`, service = `"s3"`
- Signing key chain: `HMAC("AWS4" + secret, date) → HMAC(_, "auto") → HMAC(_, "s3") → HMAC(_, "aws4_request")`
- Headers obbligatori: `Authorization`, `x-amz-date` (formato `YYYYMMDDTHHMMSSZ`), `x-amz-content-sha256`, `Host`
- Per GET: `x-amz-content-sha256` = SHA256 di stringa vuota
- Per PUT: `x-amz-content-sha256` = SHA256 del body, `Content-Type: application/octet-stream`
- Per HEAD (test): `x-amz-content-sha256` = SHA256 di stringa vuota, check solo bucket (no object_key)
- libcurl: `CURLOPT_TIMEOUT` = 30 secondi
- `r2_config_load`/`r2_config_save`: usano json-c (già linkato) per leggere/scrivere `~/.config/sshpad/r2.json`
- Creare `~/.config/sshpad/` con `mkdir -p` equivalente se non esiste
- Limitare dimensione risposta GET a 10MB (protezione OOM)

**Test di validazione**: T2.1–T2.10

---

### Agent-JSON — Serializzazione JSON ↔ ssh_host_t

**File da modificare**: `src/config_parser.c`, `src/config_parser.h`

**Contesto da fornire**:
- Sezione "Serializzazione JSON" di questo documento
- Leggere `src/config_parser.h` per la struttura `ssh_host_t` e le funzioni esistenti
- Leggere `src/http_server.c` funzione `handle_get_hosts()` per vedere il formato JSON attualmente prodotto (i nomi dei campi devono essere identici)
- json-c è già linkato

**Contratto di interfaccia (aggiunte all'header esistente)**:
```c
// Deserializza array JSON → array di ssh_host_t. Caller deve free().
// Ritorna NULL se JSON invalido.
ssh_host_t *ssh_hosts_from_json(const char *json_str, int *out_count);

// Serializza array ssh_host_t → stringa JSON. Caller deve free().
char *ssh_hosts_to_json(const ssh_host_t *hosts, int count);
```

**Dettagli implementativi**:
- I nomi dei campi JSON sono: `name`, `hostname`, `user`, `port`, `identityFile`, `proxyJump`, `localForward` (array di `{bindAddr, bindPort, remoteHost, remotePort}`), `remoteForward` (idem), `dynamicForward` (array di `{bindAddr, bindPort}`)
- `ssh_hosts_to_json()` deve produrre output identico a quello che `handle_get_hosts()` genera oggi (copiare la logica di serializzazione)
- `ssh_hosts_from_json()` deve allocare con `malloc`/`calloc`, inclusi i sotto-array forwards
- Usare `json_object_get_string()`, `json_object_get_int()`, `json_object_array_length()` etc.
- **Non** modificare le funzioni esistenti (`parse_ssh_config`, `ssh_hosts_free`, etc.)

**Test di validazione**: T3.1–T3.7

---

### Agent-SSHWRITE — Scrittura ~/.ssh/config

**File da modificare**: `src/config_parser.c`, `src/config_parser.h`

**Contesto da fornire**:
- Sezione "Scrittura ~/.ssh/config" di questo documento
- Leggere `src/config_parser.h` per la struttura `ssh_host_t`
- Formato standard SSH config (vedi esempio nel documento)

**Contratto di interfaccia (aggiunta all'header)**:
```c
// Scrive array di host come file SSH config. Crea backup .bak se il file esiste.
// Ritorna 0 OK, -1 errore.
int ssh_hosts_write_config(const ssh_host_t *hosts, int count, const char *path);
```

**Nota di coordinamento con Agent-JSON**: entrambi modificano `config_parser.c/.h`. Le aggiunte sono indipendenti (funzioni diverse). L'agente di integrazione farà il merge. Ogni agente deve aggiungere la propria funzione e il prototipo nel header **in fondo** per minimizzare conflitti.

**Dettagli implementativi**:
- Se `path` è NULL, usare `~/.ssh/config` (espandere `$HOME`)
- Prima di scrivere, se il file esiste: copiarlo come `<path>.bak`
- Creare `~/.ssh/` con permessi 0700 se non esiste
- Scrivere il file con permessi 0600
- Formato output per ogni host:
  ```
  Host <name>
      HostName <hostname>
      User <user>
      Port <port>           ← omettere se 0 o 22 (default)
      IdentityFile <path>   ← omettere se vuoto
      ProxyJump <host>      ← omettere se vuoto
      LocalForward <bindAddr>:<bindPort> <remoteHost>:<remotePort>
      RemoteForward <bindAddr>:<bindPort> <remoteHost>:<remotePort>
      DynamicForward <bindAddr>:<bindPort>
  ```
- Separare gli host con una riga vuota
- Aggiungere header commento: `# Generated by SSHPad — do not edit manually`

**Test di validazione**: T4.1–T4.6

---

### Agent-CM — Config Manager (orchestratore)

**File da creare**: `src/config_manager.h`, `src/config_manager.c`

**Contesto da fornire**:
- Sezione `src/config_manager.h / .c` di questo documento
- Header files: `crypto_store.h`, `r2_client.h`, `config_parser.h` (con le nuove aggiunte di Agent-JSON e Agent-SSHWRITE)
- Sezione "Modalità operative" e "Persistenza della scelta Solo Locale"
- Leggere `src/sse.c` per la firma di `sse_broadcast()` (o fornire il prototipo)

**Prerequisiti**: Wave 1 completata (CRYPTO, R2, JSON, SSHWRITE). L'agente riceve gli header finali.

**Contratto di interfaccia (header)**:
```c
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "config_parser.h"
#include "r2_client.h"
#include "sse.h"

typedef enum {
    CM_MODE_FIRST_RUN,
    CM_MODE_LOCKED,
    CM_MODE_CLOUD,
    CM_MODE_LOCAL
} cm_mode_t;

typedef struct config_manager config_manager_t;

// Crea il config manager. Determina la modalità iniziale.
config_manager_t *cm_create(sse_broadcaster_t *sse);

// Sblocca con password. -1 = password errata, -2 = errore rete, 0 = OK.
int cm_unlock(config_manager_t *cm, const char *password);

// Completa wizard primo avvio. mode = "cloud" o "local".
// Se cloud: r2_cfg e password obbligatori. Ritorna 0 OK, -1 errore.
int cm_setup(config_manager_t *cm, const char *mode,
             const r2_config_t *r2_cfg, const char *password);

// Ritorna modalità corrente.
cm_mode_t cm_get_mode(config_manager_t *cm);

// Ritorna array host (read-lock interno). Valido fino alla prossima chiamata di modifica.
const ssh_host_t *cm_get_hosts(config_manager_t *cm, int *out_count);

// Crea o aggiorna host. Ritorna 0 OK, -1 errore.
int cm_save_host(config_manager_t *cm, const ssh_host_t *host);

// Elimina host per nome. Ritorna 0 OK, -1 non trovato.
int cm_delete_host(config_manager_t *cm, const char *name);

// Cambia password master. Ritorna 0 OK, -1 password vecchia errata, -2 errore sync.
int cm_change_password(config_manager_t *cm, const char *old_pw, const char *new_pw);

// Ritorna config R2 corrente (secret mascherata nell'output).
void cm_get_r2_config(config_manager_t *cm, r2_config_t *out);

// Aggiorna config R2. Se in mode=CLOUD, ri-sincronizza. Ritorna 0 OK, -1 errore.
int cm_set_r2_config(config_manager_t *cm, const r2_config_t *cfg);

// Bypassa R2, carica ~/.ssh/config. Ritorna 0 OK.
int cm_use_local(config_manager_t *cm);

// Libera risorse.
void cm_free(config_manager_t *cm);

#endif
```

**Dettagli implementativi**:
- Struct interno con `pthread_rwlock_t lock`, array hosts, mode, r2_cfg, password, sse pointer
- `cm_create()`: legge `~/.config/sshpad/settings.json` con json-c. Se non esiste → FIRST_RUN. Se `mode:"local"` → parse `~/.ssh/config` → LOCAL. Se `mode:"cloud"` → carica `r2.json` → LOCKED.
- `cm_setup()`: salva `settings.json` e (se cloud) `r2.json`, poi chiama `cm_unlock()` o `parse_ssh_config()`. Broadcast SSE `config_changed`.
- `cm_save_host()` / `cm_delete_host()`: write-lock → modifica array → write-unlock → `_sync()` → SSE `config_changed`
- `_sync()` interno: se CLOUD → `ssh_hosts_to_json()` → `cs_encrypt()` → `r2_put_object()`. Se LOCAL → `ssh_hosts_write_config()`.
- `cm_set_r2_config()` in mode CLOUD: salva nuove credenziali → ri-sync → se fallisce, rollback
- `cm_get_r2_config()`: copia cfg ma maschera secret (primi 4 char + `****`)
- `settings.json` formato: `{"mode": "cloud"|"local"}`, permessi 0600
- Creare directory `~/.config/sshpad/` con 0700 se non esiste

**Test di validazione**: T5.1–T5.16

---

### Agent-API — Integrazione HTTP e build system

**File da modificare**: `src/http_server.c`, `src/main.c`, `src/app_context.h`, `CMakeLists.txt`

**Contesto da fornire**:
- Sezione "Modifiche a file C esistenti" di questo documento (app_context.h, main.c, http_server.c, CMakeLists.txt)
- Header file `config_manager.h` (contratto completo)
- Leggere i file attuali: `src/http_server.c`, `src/main.c`, `src/app_context.h`, `CMakeLists.txt`
- Tabella dei 10 nuovi endpoint

**Prerequisiti**: Wave 2 completata (CM). L'agente riceve l'header `config_manager.h` finale.

**Compiti**:

1. **`src/app_context.h`**: rimuovere `ssh_host_t *hosts` e `int num_hosts`, aggiungere `config_manager_t *cm`

2. **`src/main.c`**:
   - Aggiungere `#include <curl/curl.h>` e `#include "config_manager.h"`
   - `curl_global_init(CURL_GLOBAL_DEFAULT)` all'inizio di `main()`
   - Sostituire `parse_ssh_config()` con `cm_create(ctx.sse)`
   - Aggiornare `cleanup()`: `cm_free()` invece di `ssh_hosts_free()`
   - Aggiornare `lp_create()`: passare hosts da `cm_get_hosts()`
   - `curl_global_cleanup()` in `cleanup()`

3. **`src/http_server.c`**:
   - Aggiornare `handle_get_hosts()`, `handle_tunnel_start()`, `handle_internal_askpass()` per usare `cm_get_hosts(ctx->cm, &count)`
   - Implementare 10 nuovi handler:
     - `handle_config_status()` → GET `/api/config/status`
     - `handle_config_setup()` → POST `/api/config/setup`
     - `handle_config_unlock()` → POST `/api/config/unlock`
     - `handle_host_save()` → POST `/api/host/save`
     - `handle_host_delete()` → POST `/api/host/delete`
     - `handle_config_use_local()` → POST `/api/config/use-local`
     - `handle_config_change_password()` → POST `/api/config/change-password`
     - `handle_config_r2_settings_get()` → GET `/api/config/r2-settings`
     - `handle_config_r2_settings_post()` → POST `/api/config/r2-settings`
     - `handle_config_r2_test()` → POST `/api/config/r2-test`
   - Aggiornare il router (`answer_to_connection` o equivalente) per mappare i nuovi path
   - **Nota**: `handle_host_save()` deve parsare il JSON body in un `ssh_host_t` usando `ssh_hosts_from_json()` (wrapper per singolo host)

4. **`CMakeLists.txt`**:
   - Aggiungere `pkg_check_modules(CURL REQUIRED libcurl)`
   - Aggiungere `src/crypto_store.c`, `src/r2_client.c`, `src/config_manager.c` alla lista sorgenti
   - Aggiungere `${CURL_INCLUDE_DIRS}`, `${CURL_CFLAGS_OTHER}`, `${CURL_LIBRARIES}` ai target

**Test di validazione**: T6.1–T6.22

---

### Agent-WIZARD — Wizard primo avvio (UI)

**File da creare**: `ui/components/sshpad-wizard.js`

**Contesto da fornire**:
- Sezione "Wizard di primo avvio" e "ui/components/sshpad-wizard.js" di questo documento
- Leggere `ui/libs/lit-element-base.js` per capire la classe base `IVPLitElementBase` e il pattern `static get tag()` / `RegisterElement()`
- Leggere `ui/components/sshpad-password.js` come esempio di componente con modale Bootstrap
- Leggere `ui/main.js` per il pattern di registrazione
- Il componente usa light DOM (`createRenderRoot() { return this; }` nella classe base)
- Bootstrap 5 è disponibile: importare `import { Modal } from 'bootstrap'` se serve modale

**Contratto**:
- Tag: `sshpad-wizard`
- Properties state: `step` (1/2/3), `r2Config` (Object), `testResult`, `isNewSetup` (Boolean), `loading`, `error`
- Metodo pubblico: `show()` — mostra il wizard
- Evento emesso: `wizard-complete` con `detail: { mode: "cloud" | "local" }`
- API chiamate:
  - `POST /api/config/r2-test` (body: credenziali R2) → `{ok:true}` o `{error:"..."}`
  - `POST /api/config/setup` (body: `{mode, r2, password}` o `{mode:"local"}`) → `{ok:true}` o `{error:"..."}`

**Dettagli implementativi**:
- Step 1: due card Bootstrap centrate. Click "Inizia" → `POST /api/config/setup {mode:"local"}` → dispatch `wizard-complete`
- Step 2: form credenziali R2. "Testa Connessione" → `POST /api/config/r2-test` → feedback inline (badge verde/rosso). "Avanti" → determina `isNewSetup` tentando un GET test sull'oggetto (o il backend lo indica nella risposta del test)
- Step 3: input password (+ conferma se `isNewSetup`). "Completa" → `POST /api/config/setup {mode:"cloud", r2: this.r2Config, password}` → dispatch `wizard-complete`
- Navigazione: "← Indietro" tra gli step, preservando i dati inseriti
- Layout: fullscreen overlay (`position: fixed, inset: 0, z-index: 1060`)

---

### Agent-UNLOCK — Dialog sblocco (UI)

**File da creare**: `ui/components/sshpad-unlock.js`

**Contesto da fornire**:
- Sezione "sshpad-unlock.js" di questo documento
- Leggere `ui/components/sshpad-password.js` come esempio di modale Bootstrap con Lit
- Pattern light DOM di IVPLitElementBase

**Contratto**:
- Tag: `sshpad-unlock`
- Metodo pubblico: `show()` — apre la modale
- Eventi emessi: `unlocked`, `use-local`
- API chiamate: `POST /api/config/unlock`, `POST /api/config/use-local`
- Modale con `data-bs-backdrop="static"` e `data-bs-keyboard="false"`

---

### Agent-EDITOR — Editor host (UI)

**File da creare**: `ui/components/sshpad-host-editor.js`

**Contesto da fornire**:
- Sezione "sshpad-host-editor.js" di questo documento
- Leggere `src/config_parser.h` per la struttura `ssh_host_t` (capire i campi del form)
- Leggere `ui/components/sshpad-card.js` per il formato dati host come arriva dal server

**Contratto**:
- Tag: `sshpad-host-editor`
- Metodi pubblici: `show(host)` (edit), `showNew()` (creazione)
- Evento emesso: `host-save` con `detail: { host: {...} }`
- Il componente **non** chiama API direttamente — emette l'evento e il parent (`sshpad-app`) fa il POST

**Dettagli implementativi**:
- Form con tutti i campi di `ssh_host_t`
- Liste forwards dinamiche: array in state, +/- bottoni per aggiungere/rimuovere righe
- In edit mode: campo `name` è `readonly`
- Validazione client-side: `name` obbligatorio, formato alfanumerico + `-_. `

---

### Agent-R2SET — Impostazioni R2 (UI)

**File da creare**: `ui/components/sshpad-r2-settings.js`

**Contesto da fornire**:
- Sezione "sshpad-r2-settings.js" di questo documento
- Pattern IVPLitElementBase + modale Bootstrap

**Contratto**:
- Tag: `sshpad-r2-settings`
- Metodo pubblico: `show()` — apre la modale, carica valori da `GET /api/config/r2-settings`
- Evento emesso: `r2-saved` (dopo salvataggio riuscito)
- API chiamate: `GET /api/config/r2-settings`, `POST /api/config/r2-settings`, `POST /api/config/r2-test`

---

### Agent-CARD — Modifica sshpad-card.js

**File da modificare**: `ui/components/sshpad-card.js`

**Contesto da fornire**:
- Leggere `ui/components/sshpad-card.js` completo
- Aggiungere property `editable` (Boolean)
- Aggiungere due pulsanti in card header (solo se `editable`): matita (edit) e cestino (delete)

**Contratto**:
- Nuova property: `editable` (Boolean, default false)
- Nuovi eventi: `host-edit` con `detail: { host }`, `host-delete` con `detail: { name }`
- I pulsanti usano Bootstrap Icons: `bi-pencil`, `bi-trash`
- Non modificare le funzionalità esistenti della card

---

### Agent-INTEGRATE — Assemblaggio finale

**File da modificare**: `ui/components/sshpad-app.js`, `ui/main.js`

**Contesto da fornire**:
- Sezione "sshpad-app.js" e "main.js" di questo documento
- Leggere i file attuali: `ui/components/sshpad-app.js`, `ui/main.js`
- Tutti i contratti dei componenti UI (eventi emessi, metodi pubblici)
- Sezione "Flussi principali"

**Prerequisiti**: Wave 3 completata. Tutti i componenti UI e l'API sono pronti.

**Compiti**:
1. **`ui/main.js`**: aggiungere import di `sshpad-wizard`, `sshpad-unlock`, `sshpad-host-editor`, `sshpad-r2-settings`
2. **`ui/components/sshpad-app.js`**:
   - Nuova property state: `configMode` (string: first_run/locked/cloud/local)
   - Rivedere `_init()` (o equivalente connectedCallback):
     ```
     GET /api/config/status → configMode = response.mode
     → first_run? → this.querySelector('sshpad-wizard').show()
     → locked?    → this.querySelector('sshpad-unlock').show()
     → cloud/local? → _fetchHosts() + _connectSSE()
     ```
   - Event handler: `_onWizardComplete`, `_onUnlocked`, `_onUseLocal`, `_onHostEdit`, `_onHostDelete`, `_onHostSave`, `_onR2Saved`
   - `_onHostSave`: `POST /api/host/save` → refresh hosts (o attendere SSE `config_changed`)
   - `_onHostDelete`: confirm dialog → `POST /api/host/delete` → refresh
   - Render navbar: pulsante "+" (se mode != locked/first_run), ingranaggio (sempre), badge modalità
   - Render body: includere i 4 nuovi componenti, passare `editable=${true}` alle card
   - Ascoltare SSE `config_changed` → `_fetchHosts()` per aggiornare dopo modifiche

---

### Tabella riassuntiva agenti

| Agente | Wave | File output | Dipende da | Durata stimata |
|--------|------|-------------|------------|----------------|
| Agent-CRYPTO | 1 | crypto_store.c/.h | — | Bassa |
| Agent-R2 | 1 | r2_client.c/.h | — | Media (Sig V4) |
| Agent-JSON | 1 | config_parser.c/.h (aggiunte) | — | Bassa |
| Agent-SSHWRITE | 1 | config_parser.c/.h (aggiunte) | — | Bassa |
| Agent-CM | 2 | config_manager.c/.h | CRYPTO, R2, JSON, SSHWRITE | Media |
| Agent-API | 3 | http_server.c, main.c, app_context.h, CMakeLists.txt | CM | Alta |
| Agent-WIZARD | 3 | sshpad-wizard.js | — (solo contratto API) | Media |
| Agent-UNLOCK | 3 | sshpad-unlock.js | — | Bassa |
| Agent-EDITOR | 3 | sshpad-host-editor.js | — | Media |
| Agent-R2SET | 3 | sshpad-r2-settings.js | — | Bassa |
| Agent-CARD | 3 | sshpad-card.js (modifiche) | — | Bassa |
| Agent-INTEGRATE | 4 | sshpad-app.js, main.js | Tutti Wave 3 | Media |

### Prompt template per ogni agente

Ogni agente Sonnet deve ricevere un prompt strutturato così:

```
## Ruolo
Sei Agent-{NOME}. Il tuo compito è implementare {descrizione breve}.

## File su cui operi
- Creare: {lista file da creare}
- Modificare: {lista file da modificare}
- NON toccare altri file.

## Contratto di interfaccia
{header .h con signature esatte — da rispettare senza modifiche}

## Contesto del progetto
- SSHPad è un'app desktop C con UI HTML/Lit via WebKitGTK
- Build system: cmake, dipendenze: GTK4, WebKitGTK, libmicrohttpd, json-c, OpenSSL, libcurl
- Frontend: Lit Web Components con light DOM, Bootstrap 5 (importato via Vite)
- Classe base UI: IVPLitElementBase (vedi libs/lit-element-base.js)
- Lingua: commenti e stringhe UI in italiano

## Specifiche dettagliate
{copia della sezione rilevante dal documento}

## File di riferimento da leggere
{lista file esistenti che l'agente deve leggere per capire pattern e convenzioni}

## Test di validazione
{lista test T*.* che verificano il suo output}

## Output atteso
Codice completo e compilabile per i file elencati. Nessun placeholder, nessun TODO.
```

### Gestione merge di Agent-JSON e Agent-SSHWRITE

Entrambi aggiungono funzioni a `config_parser.c/.h`. Per evitare conflitti:
- Agent-JSON aggiunge i suoi prototipi **dopo** le funzioni esistenti nel .h, con commento `// --- JSON serialization ---`
- Agent-SSHWRITE aggiunge il suo prototipo **in fondo** al .h, con commento `// --- SSH config writer ---`
- Nel .c: ogni agente aggiunge le sue funzioni in fondo al file
- L'agente di integrazione (o l'operatore) fa il merge manuale dei due diff, che non dovrebbero avere conflitti reali

### Verifica post-integrazione

Dopo che Agent-INTEGRATE ha completato l'assemblaggio:

1. `npm run rebuild` — deve compilare senza errori
2. `npm run ui:build` — deve bundlare senza errori
3. Eseguire i test T6 (API) con curl
4. Eseguire i test T7–T11 (UI) manualmente
5. Eseguire i test T12 (end-to-end)
6. Eseguire i test T13 (sicurezza)
