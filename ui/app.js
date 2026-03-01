// --- State ---
let hosts = [];
let tunnelStates = {};
let pendingPasswordRequest = null;

// --- API ---
async function fetchHosts() {
  try {
    const res = await fetch('/api/hosts');
    hosts = await res.json();
    hosts.forEach(h => {
      if (!tunnelStates[h.name]) tunnelStates[h.name] = h.tunnelStatus || 'inactive';
    });
    render();
  } catch (e) {
    console.error('fetchHosts error:', e);
  }
}

async function startTunnel(hostName) {
  tunnelStates[hostName] = 'starting';
  render();
  try {
    await fetch('/api/tunnel/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: hostName }),
    });
  } catch (e) {
    console.error('startTunnel error:', e);
    tunnelStates[hostName] = 'error';
    render();
  }
}

async function stopTunnel(hostName) {
  try {
    await fetch('/api/tunnel/stop', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: hostName }),
    });
  } catch (e) {
    console.error('stopTunnel error:', e);
  }
}

async function openTerminal(hostName) {
  try {
    await fetch('/api/terminal/open', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host: hostName }),
    });
  } catch (e) {
    console.error('openTerminal error:', e);
  }
}

async function sendPassword(requestId, password) {
  try {
    await fetch('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ requestId, password }),
    });
  } catch (e) {
    console.error('sendPassword error:', e);
  }
}

// --- SSE ---
function connectSSE() {
  const es = new EventSource('/api/events');
  const indicator = document.getElementById('status-indicator');

  es.onopen = () => { indicator.className = 'status-dot active'; };
  es.onerror = () => {
    indicator.className = 'status-dot error';
    setTimeout(connectSSE, 3000);
    es.close();
  };

  es.addEventListener('tunnel_status', (e) => {
    const data = JSON.parse(e.data);
    tunnelStates[data.host] = data.status;
    render();
  });

  es.addEventListener('password_request', (e) => {
    const data = JSON.parse(e.data);
    showPasswordDialog(data.requestId, data.host, data.prompt);
  });

  es.addEventListener('config_changed', () => {
    fetchHosts();
  });
}

// --- Password Dialog ---
const dialog = document.getElementById('password-dialog');
const form = document.getElementById('password-form');
const promptEl = document.getElementById('password-prompt');
const input = document.getElementById('password-input');
const cancelBtn = document.getElementById('password-cancel');

function showPasswordDialog(requestId, host, prompt) {
  pendingPasswordRequest = requestId;
  promptEl.textContent = prompt || `Password per ${host}:`;
  input.value = '';
  dialog.showModal();
  input.focus();
}

form.addEventListener('submit', (e) => {
  e.preventDefault();
  if (pendingPasswordRequest) {
    sendPassword(pendingPasswordRequest, input.value);
    pendingPasswordRequest = null;
  }
  dialog.close();
});

cancelBtn.addEventListener('click', () => {
  pendingPasswordRequest = null;
  dialog.close();
});

// --- Render ---
function render() {
  const container = document.getElementById('connections');

  if (hosts.length === 0) {
    container.innerHTML = '<div id="empty-state">Nessun host trovato in ~/.ssh/config</div>';
    return;
  }

  container.innerHTML = hosts.map(h => {
    const status = tunnelStates[h.name] || 'inactive';
    const hasTunnels = (h.localForward || []).length + (h.remoteForward || []).length + (h.dynamicForward || []).length > 0;

    const forwards = [
      ...(h.localForward || []).map(f => ({ type: 'L', label: `${f.bindAddr}:${f.bindPort} \u2192 ${f.remoteHost}:${f.remotePort}` })),
      ...(h.remoteForward || []).map(f => ({ type: 'R', label: `${f.bindAddr}:${f.bindPort} \u2192 ${f.remoteHost}:${f.remotePort}` })),
      ...(h.dynamicForward || []).map(f => ({ type: 'D', label: `${f.bindAddr}:${f.bindPort} (SOCKS)` })),
    ];

    const isOn      = status === 'active' || status === 'starting';
    const isStarting = status === 'starting';
    const toggleFn  = isOn ? `stopTunnel('${esc(h.name)}')` : `startTunnel('${esc(h.name)}')`;

    return `
      <div class="card">
        <div class="card-header">
          <span class="card-name">
            <span class="status-dot ${status}"></span>
            ${esc(h.name)}
          </span>
          <div class="card-actions">
            <button class="btn-terminal" onclick="openTerminal('${esc(h.name)}')" title="Apri terminale">
              &gt;_
            </button>
            <label class="toggle" title="${isOn ? 'Disconnetti' : 'Connetti'}">
              <input type="checkbox"
                     ${isOn ? 'checked' : ''}
                     ${isStarting ? 'disabled' : ''}
                     onchange="${toggleFn}">
              <span class="toggle-slider ${isStarting ? 'starting' : ''}"></span>
            </label>
          </div>
        </div>
        <div class="card-meta">
          ${esc(h.user || '')}${h.user ? '@' : ''}${esc(h.hostname)}:${h.port || 22}
          ${h.identityFile ? `<br/>Key: ${esc(h.identityFile)}` : ''}
          ${h.proxyJump ? `<br/>Via: ${esc(h.proxyJump)}` : ''}
        </div>
        ${forwards.length > 0 ? `
          <div class="tunnels">
            ${forwards.map(f => `
              <div class="tunnel-row">
                <span><span class="tunnel-type">${f.type}</span> ${esc(f.label)}</span>
                <span class="status-dot ${status}"></span>
              </div>
            `).join('')}
          </div>
        ` : ''}
      </div>
    `;
  }).join('');
}

function esc(s) {
  if (!s) return '';
  const el = document.createElement('span');
  el.textContent = s;
  return el.innerHTML;
}

// --- Init ---
fetchHosts();
connectSSE();
