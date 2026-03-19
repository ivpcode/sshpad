// SSHPad - Main entry point

// CSS (Vite li inietta nel bundle)
import 'bootstrap/dist/css/bootstrap.min.css';
import 'bootstrap-icons/font/bootstrap-icons.min.css';
import './style.css';


// Componenti (la registrazione avviene all'import)
import './libs/lit-element-base.js';
import './components/sshpad-status-dot.js';
import './components/sshpad-forwards.js';
import './components/sshpad-card.js';
import './components/sshpad-password.js';
import './components/sshpad-toast.js';
import './components/sshpad-app.js';
import './components/sshpad-wizard.js';
import './components/sshpad-unlock.js';
import './components/sshpad-host-editor.js';
import './components/sshpad-r2-settings.js';

console.info('SSHPad UI loaded');
