// ════════════════════════════════════════════════════════════════
//  MediCart — shared helpers: MQTT, vitals, WebRTC, UI bits
//  Firebase itself is loaded by the small module at the end of each page.
// ════════════════════════════════════════════════════════════════
(function () {
const { MQTT } = window.MC_CONFIG;

// ─── Server clock (presence ages are computed against it) ───────
let serverOffset = 0;
function initServerClock(fb) {
  fb.onValue(fb.ref(fb.rtdb, '.info/serverTimeOffset'), (s) => { serverOffset = Number(s.val()) || 0; });
}
const serverNow = () => Date.now() + serverOffset;

// ─── Sensor payload ─────────────────────────────────────────────
function normalizeSensor(raw) {
  let d;
  try { d = typeof raw === 'string' ? JSON.parse(raw) : raw; } catch { return null; }
  if (!d || typeof d !== 'object') return null;
  const num = (v) => (Number.isFinite(Number(v)) ? Number(v) : 0);
  const finger = Boolean(d.fingerDetected ?? d.finger ?? false);
  return {
    heartRate: num(d.heartRate ?? d.hr),
    spo2: num(d.spo2 ?? d.SpO2),
    ir: num(d.ir ?? d.IR),
    red: num(d.red ?? d.RED),
    fingerDetected: finger,
    state: String(d.state ?? (finger ? 'measuring' : 'no_finger')),
    samplesCollected: num(d.samplesCollected ?? d.samples),
    deviceId: String(d.deviceId ?? 'MEDICART-01'),
    uptime: num(d.uptime),
    at: Date.now(),
  };
}

// Indicative bands for display only — not a diagnosis.
function classifyHR(hr) {
  if (!hr) return { label: 'No reading', tone: 'neutral' };
  if (hr < 50) return { label: 'Low', tone: 'crit' };
  if (hr < 60) return { label: 'Below typical', tone: 'warn' };
  if (hr <= 100) return { label: 'Typical', tone: 'ok' };
  if (hr <= 120) return { label: 'Elevated', tone: 'warn' };
  return { label: 'High', tone: 'crit' };
}
function classifySpO2(sp) {
  if (!sp) return { label: 'No reading', tone: 'neutral' };
  if (sp >= 95) return { label: 'Typical', tone: 'ok' };
  if (sp >= 90) return { label: 'Low', tone: 'warn' };
  return { label: 'Very low', tone: 'crit' };
}
const STATE_LABELS = {
  no_finger: 'No finger',
  waiting: 'Waiting for signal',
  measuring: 'Measuring',
  locked: 'Reading complete',
  sensor_error: 'Sensor error',
  offline: 'Offline',
};
const stateLabel = (s) => STATE_LABELS[s] || String(s || '—').replace(/_/g, ' ');

// ─── MQTT ───────────────────────────────────────────────────────
function connectMqtt(h = {}) {
  if (!window.mqtt) { h.onError?.(new Error('MQTT library did not load')); return null; }
  const clientId = MQTT.clientPrefix + Math.random().toString(16).slice(2, 10);
  const client = window.mqtt.connect(MQTT.url, {
    ...(MQTT.username ? { username: MQTT.username, password: MQTT.password } : {}),
    clientId,
    clean: true,
    keepalive: 30,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });
  client.on('connect', () => {
    client.subscribe([MQTT.topics.sensors, MQTT.topics.status], { qos: 0 });
    h.onConnect?.(clientId);
  });
  client.on('reconnect', () => h.onReconnect?.());
  client.on('close', () => h.onClose?.());
  client.on('error', (e) => h.onError?.(e));
  client.on('message', (topic, payload) => {
    const text = payload.toString();
    h.onMessage?.(topic, text);
    if (topic === MQTT.topics.sensors) {
      const d = normalizeSensor(text);
      if (d) h.onSensor?.(d);
    } else if (topic === MQTT.topics.status) {
      try { h.onStatus?.(JSON.parse(text)); } catch { /* non-JSON status ignored */ }
    }
  });
  return client;
}

// ─── WebRTC helpers ─────────────────────────────────────────────
// RTDB rejects `undefined`; strip it and tag candidates with the call id.
const packCandidate = (c, callId) => JSON.parse(JSON.stringify({ ...c.toJSON(), callId }));
function unpackCandidate(v) {
  const { callId, ...init } = v || {};
  return init;
}
async function readStats(pc) {
  if (!pc) return null;
  const out = { rtt: null, fps: null, relay: false, lost: 0 };
  const stats = await pc.getStats();
  let pair = null;
  stats.forEach((r) => {
    if (r.type === 'candidate-pair' && r.state === 'succeeded' && (r.nominated || r.selected)) pair = r;
    if (r.type === 'inbound-rtp' && r.kind === 'video') {
      out.fps = r.framesPerSecond ?? out.fps;
      out.lost += r.packetsLost || 0;
    }
  });
  if (pair) {
    out.rtt = pair.currentRoundTripTime != null ? Math.round(pair.currentRoundTripTime * 1000) : null;
    const local = stats.get(pair.localCandidateId);
    const remote = stats.get(pair.remoteCandidateId);
    out.relay = local?.candidateType === 'relay' || remote?.candidateType === 'relay';
  }
  return out;
}

// ─── Small UI utilities ─────────────────────────────────────────
const el = (id) => document.getElementById(id);

function escapeHTML(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmtClock(totalSec) {
  const s = Math.max(0, Math.floor(totalSec));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(r)}` : `${pad(m)}:${pad(r)}`;
}

function fmtAgo(ms) {
  if (ms == null || !Number.isFinite(ms)) return '—';
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  return `${Math.floor(s / 3600)}h ago`;
}

function toDate(v) {
  if (!v) return null;
  if (typeof v.toDate === 'function') return v.toDate();
  const d = new Date(v);
  return Number.isNaN(d.getTime()) ? null : d;
}

function toast(text, tone = 'info', ms = 4200) {
  const host = el('toasts');
  if (!host) return;
  const t = document.createElement('div');
  t.className = `toast toast-${tone}`;
  t.textContent = text;
  host.appendChild(t);
  requestAnimationFrame(() => t.classList.add('in'));
  setTimeout(() => {
    t.classList.remove('in');
    setTimeout(() => t.remove(), 260);
  }, ms);
}

function toCSV(rows) {
  if (!rows.length) return '';
  const keys = Object.keys(rows[0]);
  const cell = (v) => {
    const s = v == null ? '' : String(v);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  return [keys.join(','), ...rows.map((r) => keys.map((k) => cell(r[k])).join(','))].join('\n');
}

function download(filename, text, mime = 'text/plain') {
  const blob = new Blob([text], { type: `${mime};charset=utf-8` });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}

function authErrorText(err) {
  const code = err?.code || '';
  if (code.includes('invalid-credential') || code.includes('wrong-password') || code.includes('user-not-found')) return 'Email or password is incorrect.';
  if (code.includes('too-many-requests')) return 'Too many attempts. Wait a minute and try again.';
  if (code.includes('network-request-failed')) return 'No internet connection.';
  if (code.includes('invalid-email')) return 'That email address is not valid.';
  if (code.includes('user-disabled')) return 'This account has been disabled.';
  return err?.message || 'Sign-in failed.';
}

window.MC = {
  initServerClock,
  serverNow,
  normalizeSensor,
  classifyHR,
  classifySpO2,
  STATE_LABELS,
  stateLabel,
  connectMqtt,
  packCandidate,
  unpackCandidate,
  readStats,
  el,
  escapeHTML,
  fmtClock,
  fmtAgo,
  toDate,
  toast,
  toCSV,
  download,
  authErrorText,
};
})();
