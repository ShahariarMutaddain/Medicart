// ════════════════════════════════════════════════════════════════
//  MediCart Console — clinician side
//  Auth · live vitals (MQTT) · records (Firestore) · WebRTC caller
// ════════════════════════════════════════════════════════════════
(function () {
// Runs once the Firebase loader at the end of the page has finished.
function main() {
const {
  auth, rtdb, db, onAuthStateChanged, signInWithEmailAndPassword, signOut, sendPasswordResetEmail,
  updateProfile, ref, set, update, onValue, onChildAdded, onDisconnect, serverTimestamp, push,
  collection, addDoc, getDocs, query, orderBy, limit, writeBatch, fsTimestamp, Timestamp,
} = window.MC_FB;
const {
  connectMqtt, classifyHR, classifySpO2, stateLabel, serverNow, packCandidate, unpackCandidate,
  readStats, el, toast, fmtClock, fmtAgo, toDate, toCSV, download, escapeHTML, authErrorText,
} = window.MC;
const {
  CART_ID, CART_LABEL, ICE_SERVERS, DOCTOR_VIDEO, MQTT, PROMPTS, FIREBASE_CONFIG, RING_TIMEOUT_MS,
  PRESENCE_TIMEOUT_MS, SENSOR_STALE_MS,
} = window.MC_CONFIG;

// ─── State ──────────────────────────────────────────────────────
const S = {
  user: null,
  started: false,
  mqtt: null,
  mqttOn: false,
  mqttClientId: null,
  reconnects: 0,
  msgCount: 0,
  rateWindow: 0,
  rate: 0,
  demo: null,
  vit: null,             // latest normalized sensor payload
  lastSensorAt: 0,
  device: null,          // ESP32 status topic
  cart: null,            // RTDB presence
  chartWin: 60,
  series: { hr: [], sp: [], ir: [], red: [] },
  raw: [],
  rawPaused: false,
  lastMsg: null,
  consults: [],
  history: [],
  histN: 100,
  call: null,
};

const OUTCOME = {
  completed: ['Completed', 'ok'],
  no_answer: ['No answer', 'warn'],
  declined: ['Declined at cart', 'warn'],
  cancelled: ['Cancelled', 'neutral'],
  failed: ['Connection failed', 'crit'],
  ended_by_cart: ['Ended at cart', 'ok'],
  cart_lost: ['Cart disconnected', 'crit'],
};

// ─── Auth gate ──────────────────────────────────────────────────
onAuthStateChanged(auth, (user) => {
  el('boot').hidden = true;
  S.user = user;
  if (!user) {
    if (S.started) { location.hash = ''; location.reload(); return; }
    el('app').hidden = true;
    el('login').hidden = false;
    setTimeout(() => el('loginEmail')?.focus(), 50);
    return;
  }
  el('login').hidden = true;
  el('app').hidden = false;
  if (!S.started) startApp();
  renderUser();
});

el('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const email = el('loginEmail').value.trim();
  const pass = el('loginPass').value;
  const msg = el('loginMsg');
  msg.hidden = true; msg.className = 'form-msg';
  if (!email || !pass) { msg.textContent = 'Enter your email and password.'; msg.hidden = false; return; }
  const btn = el('loginBtn');
  btn.disabled = true; btn.textContent = 'Signing in…';
  try {
    await signInWithEmailAndPassword(auth, email, pass);
  } catch (err) {
    msg.textContent = authErrorText(err); msg.hidden = false;
  } finally {
    btn.disabled = false; btn.textContent = 'Sign in';
  }
});

el('forgotBtn').addEventListener('click', async () => {
  const email = el('loginEmail').value.trim();
  const msg = el('loginMsg');
  if (!email) { msg.className = 'form-msg'; msg.textContent = 'Type your email above first, then tap “Forgot password?”.'; msg.hidden = false; return; }
  try {
    await sendPasswordResetEmail(auth, email);
    msg.className = 'form-msg ok'; msg.textContent = `If ${email} has an account, a reset link is on its way.`; msg.hidden = false;
  } catch (err) {
    msg.className = 'form-msg'; msg.textContent = authErrorText(err); msg.hidden = false;
  }
});

el('signOutBtn').addEventListener('click', async () => {
  if (S.call) await hangup('cancelled');
  await signOut(auth);
});

const doctorName = () => S.user?.displayName || (S.user?.email || 'Clinician').split('@')[0];

function renderUser() {
  el('userName').textContent = doctorName();
  el('userEmail').textContent = S.user.email || '';
  el('sysEmail').textContent = S.user.email || '—';
  el('sysUid').textContent = S.user.uid;
  el('profileName').value = S.user.displayName || '';
  const h = new Date().getHours();
  const part = h < 12 ? 'Good morning' : h < 17 ? 'Good afternoon' : 'Good evening';
  el('greetText').textContent = `${part}, ${doctorName()}`;
  el('greetDate').textContent = new Date().toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });
}

// ─── Boot the app once signed in ────────────────────────────────
function startApp() {
  S.started = true;
  el('nodeCartName').textContent = CART_LABEL;
  el('sysCartId').textContent = CART_ID;
  el('sysProject').textContent = FIREBASE_CONFIG.projectId;
  el('sysBroker').textContent = MQTT.url.replace('wss://', '').replace(/\/mqtt$/, '');
  el('sysSignal').textContent = `/calls/${CART_ID}`;

  initNav();
  initCharts();
  initConsult();
  initRecords();
  initStream();
  initSystem();
  initDrive();
  startMqtt();

  onValue(ref(rtdb, `carts/${CART_ID}`), (snap) => { S.cart = snap.val(); renderLinks(); });
  setInterval(tick, 1000);
  loadConsultations();
  tick();
}

// ─── Navigation (hash routes) ───────────────────────────────────
const PAGES = ['overview', 'vitals', 'consult', 'records', 'drive', 'stream', 'system'];
function initNav() {
  const go = () => {
    const id = PAGES.includes(location.hash.slice(1)) ? location.hash.slice(1) : 'overview';
    document.querySelectorAll('.page').forEach((p) => p.classList.toggle('on', p.id === `page-${id}`));
    document.querySelectorAll('#nav a').forEach((a) => a.classList.toggle('on', a.dataset.page === id));
    el('pageTitle').textContent = el(`page-${id}`).dataset.title;
    closeRail();
    if (id === 'records' && !el('tab-history').hidden && !S.history.length) loadHistory();
    if (id === 'vitals') Object.values(charts).forEach((c) => c.resize());
    if (id !== 'drive') releaseDrive();   // leaving the Drive page always stops manual driving
  };
  window.addEventListener('hashchange', go);
  go();
  el('menuBtn').addEventListener('click', () => { el('rail').classList.add('open'); el('scrim').classList.add('open'); });
  el('scrim').addEventListener('click', closeRail);
}
function closeRail() { el('rail').classList.remove('open'); el('scrim').classList.remove('open'); }

// ─── MQTT live link ─────────────────────────────────────────────
function startMqtt() {
  if (S.mqtt) return;
  S.mqtt = connectMqtt({
    onConnect: (id) => { S.mqttOn = true; S.mqttClientId = id; renderLinks(); },
    onReconnect: () => { S.reconnects += 1; },
    onClose: () => { S.mqttOn = false; renderLinks(); },
    onError: (e) => console.warn('[mqtt]', e?.message || e),
    onMessage: (topic, text) => {
      S.msgCount += 1; S.rateWindow += 1;
      S.lastMsg = { topic, text, at: Date.now() };
      renderInspector();
    },
    onSensor: (d) => { if (!S.demo) ingest(d, 'MQTT'); },
    onStatus: (s) => { S.device = { ...s, at: Date.now() }; renderDevice(); renderLinks(); },
  });
  el('mqttToggle').textContent = 'Disconnect';
}
function stopMqtt() {
  if (!S.mqtt) return;
  S.mqtt.end(true);
  S.mqtt = null; S.mqttOn = false;
  el('mqttToggle').textContent = 'Connect';
  renderLinks();
}

// ─── Demo data ──────────────────────────────────────────────────
el('demoBtn').addEventListener('click', () => {
  if (S.demo) {
    clearInterval(S.demo); S.demo = null;
    el('demoBtn').textContent = 'Demo data';
    toast('Demo stopped — showing live data.');
    renderLinks();
    return;
  }
  let hr = 76, sp = 97, ir = 64000, red = 51000, n = 0, finger = true;
  S.demo = setInterval(() => {
    n += 1;
    if (n % 45 === 0) finger = !finger;
    hr = Math.min(108, Math.max(62, hr + (Math.random() - 0.5) * 2.4));
    sp = Math.min(99, Math.max(94, sp + (Math.random() - 0.48) * 0.5));
    ir = Math.min(88000, Math.max(52000, ir + (Math.random() - 0.5) * 1400));
    red = Math.min(68000, Math.max(36000, red + (Math.random() - 0.5) * 1100));
    const samples = finger ? Math.min(4, (n % 45) >> 1) : 0;
    const locked = finger && samples >= 4;
    ingest({
      heartRate: locked ? Math.round(hr) : 0, spo2: locked ? Math.round(sp) : 0,
      ir: finger ? Math.round(ir) : 4200, red: finger ? Math.round(red) : 3100,
      fingerDetected: finger, state: !finger ? 'no_finger' : locked ? 'locked' : 'measuring',
      samplesCollected: samples, deviceId: 'MEDICART-01', uptime: n, at: Date.now(),
    }, 'Demo');
  }, 1000);
  el('demoBtn').textContent = 'Stop demo';
  toast('Demo data on — simulated readings, nothing is saved.', 'warn');
});

// ─── Ingest one sensor packet ───────────────────────────────────
function ingest(d, source) {
  S.vit = { ...d, source };
  S.lastSensorAt = Date.now();
  pushSeries(d);
  pushRaw(d);
  renderVitals();
  renderLinks();
}

function renderVitals() {
  const d = S.vit;
  if (!d) return;
  const hr = d.fingerDetected && d.heartRate > 0 ? d.heartRate : null;
  const sp = d.fingerDetected && d.spo2 > 0 ? d.spo2 : null;
  const cH = classifyHR(hr), cS = classifySpO2(sp);
  const setPill = (id, c) => { const p = el(id); p.className = `pill ${c.tone === 'neutral' ? '' : c.tone}`; p.textContent = c.label; };

  for (const id of ['kHR', 'vHR', 'cvHR']) el(id).textContent = hr ?? '--';
  for (const id of ['kSp', 'vSp', 'cvSp']) el(id).textContent = sp ?? '--';
  setPill('kHRPill', cH); setPill('vHRPill', cH);
  setPill('kSpPill', cS); setPill('vSpPill', cS);

  el('kIR').textContent = d.ir ? d.ir.toLocaleString() : '--';
  const irP = el('kIRPill');
  irP.className = `pill ${d.fingerDetected ? 'ok' : ''}`;
  irP.textContent = d.fingerDetected ? 'Finger on' : 'No finger';

  const st = stateLabel(d.state);
  el('kState').textContent = st;
  el('cvState').textContent = st;
  el('cvState').className = `pill ${d.state === 'locked' ? 'ok' : d.state === 'measuring' ? 'info' : d.state === 'sensor_error' ? 'crit' : ''}`;
  el('kSteps').querySelectorAll('i').forEach((i, k) => i.classList.toggle('on', k < d.samplesCollected));

  el('vFinger').textContent = d.fingerDetected ? 'Detected' : 'Not detected';
  el('vState').textContent = st;
  el('vSamples').textContent = `${d.samplesCollected} / 4`;
  el('vSource').textContent = d.source;
}

// ─── Links: cart presence, sensor, MQTT ─────────────────────────
const cartOnline = () => !!(S.cart?.online && serverNow() - (S.cart.lastSeen || 0) < PRESENCE_TIMEOUT_MS);
const sensorOnline = () => !!S.demo || (Date.now() - S.lastSensorAt < SENSOR_STALE_MS && S.device?.status !== 'offline');

function renderLinks() {
  const cart = cartOnline();
  const sensor = sensorOnline();
  const link = S.mqttOn || !!S.demo;
  const finger = sensor && S.vit?.fingerDetected;

  const dot = (id, tone, live) => { el(id).className = `dot ${tone}${live ? ' live' : ''}`; };
  dot('rsCart', cart ? 'ok' : 'crit', cart);
  el('rsCartText').textContent = cart ? (S.cart.inCall ? `${CART_LABEL} · in call` : `${CART_LABEL} online`) : 'Cart offline';
  dot('rsSensor', sensor ? (S.demo ? 'warn' : 'ok') : 'crit');
  el('rsSensorText').textContent = sensor ? (S.demo ? 'Sensor simulated' : 'Sensor streaming') : 'Sensor offline';
  dot('rsLink', link ? 'ok' : 'crit', link);
  el('rsLinkText').textContent = S.demo ? 'Demo mode' : S.mqttOn ? 'Live link on' : 'Live link off';

  const pill = el('linkPill');
  pill.className = `pill ${S.demo ? 'warn' : S.mqttOn ? 'ok' : 'crit'}`;
  el('linkPillText').textContent = S.demo ? 'DEMO' : S.mqttOn ? 'LIVE' : 'OFFLINE';

  // care loop
  el('nodeCart').className = `node ${cart ? 'is-on' : ''}`;
  el('nodeCartText').textContent = cart ? (S.cart.inCall ? 'In a call' : 'Screen online') : 'Offline';
  el('edgeLink').className = `edge ${link ? 'is-on' : ''}`;
  el('edgeLinkText').textContent = S.demo ? 'Demo' : 'Cloud link';
  el('edgeSensor').className = `edge ${sensor ? 'is-on' : ''}`;
  el('edgeSensorText').textContent = sensor ? 'Sensor live' : 'Sensor off';
  el('nodePatient').className = `node ${finger ? 'is-on' : sensor ? 'is-warn' : ''}`;
  el('nodePatientText').textContent = finger ? stateLabel(S.vit.state) : sensor ? 'Not on sensor' : 'Unknown';

  if (!sensor && S.vit && !S.demo) { S.vit = null; clearVitals(); }
  renderCartSystem();
  renderCallAvailability();
}

function clearVitals() {
  for (const id of ['kHR', 'vHR', 'cvHR', 'kSp', 'vSp', 'cvSp', 'kIR']) el(id).textContent = '--';
  for (const id of ['kHRPill', 'vHRPill', 'kSpPill', 'vSpPill']) { el(id).className = 'pill'; el(id).textContent = 'No reading'; }
  el('kIRPill').className = 'pill'; el('kIRPill').textContent = '—';
  el('kState').textContent = 'Offline'; el('cvState').textContent = 'Offline'; el('cvState').className = 'pill';
  el('kSteps').querySelectorAll('i').forEach((i) => i.classList.remove('on'));
  el('vFinger').textContent = '—'; el('vState').textContent = 'Offline'; el('vSamples').textContent = '0 / 4'; el('vSource').textContent = '—';
}

function tick() {
  el('vLast').textContent = S.lastSensorAt ? fmtAgo(Date.now() - S.lastSensorAt) : '—';
  S.rate = S.rateWindow; S.rateWindow = 0;
  el('sysMsgs').textContent = S.msgCount.toLocaleString();
  el('sysRate').textContent = `${S.rate} /s`;
  el('sysRecon').textContent = S.reconnects;
  el('sysClient').textContent = S.mqttClientId || '—';
  const mp = el('sysMqttPill');
  mp.className = `pill ${S.mqttOn ? 'ok' : 'crit'}`; mp.textContent = S.mqttOn ? 'Connected' : 'Disconnected';
  renderLinks();
  renderDevice();
  renderDrive();
  if (S.call?.connectedAt) el('callTimer').textContent = fmtClock((Date.now() - S.call.connectedAt) / 1000);
}

function renderDevice() {
  const d = S.device;
  const on = sensorOnline();
  const p = el('sysSensorPill');
  p.className = `pill ${S.demo ? 'warn' : on ? 'ok' : 'crit'}`;
  p.textContent = S.demo ? 'Simulated' : on ? 'Online' : 'Offline';
  if (!d) return;
  el('sysDev').textContent = d.device || 'MEDICART-01';
  el('sysFw').textContent = d.fw || '—';
  el('sysIp').textContent = d.ip || '—';
  el('sysWifi').textContent = d.ssid ? `${d.ssid}${d.rssi ? ` · ${d.rssi} dBm` : ''}` : '—';
  el('sysMax').textContent = d.sensor === false ? 'Not detected' : d.sensor === true ? 'OK' : '—';
  el('sysUptime').textContent = S.vit?.uptime ? fmtClock(S.vit.uptime) : '—';
}

function renderCartSystem() {
  const c = S.cart, on = cartOnline();
  const p = el('sysCartPill');
  p.className = `pill ${on ? 'ok' : 'crit'}`; p.textContent = on ? 'Online' : 'Offline';
  el('sysCartSeen').textContent = c?.lastSeen ? fmtAgo(serverNow() - c.lastSeen) : '—';
  el('sysCartCam').textContent = c?.camera || (c ? 'No camera' : '—');
  el('sysCartMic').textContent = c?.mic || (c ? 'No microphone' : '—');
  el('sysCartCall').textContent = c?.inCall ? `Yes${c.callDoctor ? ` · ${c.callDoctor}` : ''}` : 'No';
  el('sysCartVer').textContent = c?.version || '—';
}

// ════════════════════════════════════════════════════════════════
//  Drive — manual + autonomous control (robot side: medicart-control on the Pi)
//  carts/<id>/control/request  {mode AUTO|MANUAL|STOP, by, name, id, at}   ← console / SSH
//  carts/<id>/control/drive    {l, r, seq, by, name, at}  every 150 ms while held
//  carts/<id>/control/state    written by the Pi once a second
// ════════════════════════════════════════════════════════════════
const ctlPath = `carts/${CART_ID}/control`;
const DRV = { state: null, request: null, held: false, keys: new Set(), x: 0, y: 0, l: 0, r: 0, seq: 0, timer: null, disc: false, speed: 120 };
const KEYMAP = { KeyW: 'f', ArrowUp: 'f', KeyS: 'b', ArrowDown: 'b', KeyA: 'l', ArrowLeft: 'l', KeyD: 'r', ArrowRight: 'r' };

function myName() { return S.user?.displayName || S.user?.email || 'Console'; }
function ctlOnline() { const st = DRV.state; return !!(st && st.updatedAt && serverNow() - st.updatedAt < 5000); }
function canDrive() {
  const st = DRV.state;
  return ctlOnline() && st.mode === 'MANUAL' && st.manualCapable && st.arduino === 'online';
}

function initDrive() {
  onValue(ref(rtdb, `${ctlPath}/state`), (snap) => { DRV.state = snap.val(); renderDrive(); });
  onValue(ref(rtdb, `${ctlPath}/request`), (snap) => { DRV.request = snap.val(); renderDrive(); });
  el('modeAuto').addEventListener('click', () => requestMode('AUTO'));
  el('modeManual').addEventListener('click', () => requestMode('MANUAL'));
  el('estopBtn').addEventListener('click', () => requestMode('STOP'));

  try { const v = parseInt(localStorage.getItem('mc-drive-speed') || '', 10); if (v >= 40 && v <= 200) DRV.speed = v; } catch { /* storage blocked */ }
  const spd = el('spd');
  spd.value = DRV.speed; el('spdVal').textContent = DRV.speed;
  spd.addEventListener('input', () => {
    DRV.speed = +spd.value; el('spdVal').textContent = DRV.speed;
    try { localStorage.setItem('mc-drive-speed', String(DRV.speed)); } catch { /* ignore */ }
    if (DRV.held) setTarget(DRV.x, DRV.y);
  });

  const pad = el('pad');
  const move = (e) => {
    const r = pad.getBoundingClientRect();
    const R = r.width / 2 * 0.74;
    let dx = (e.clientX - (r.left + r.width / 2)) / R;
    let dy = (e.clientY - (r.top + r.height / 2)) / R;
    const m = Math.hypot(dx, dy);
    if (m > 1) { dx /= m; dy /= m; }
    setKnob(dx, dy);
    setTarget(dx, -dy);
  };
  pad.addEventListener('pointerdown', (e) => {
    if (!canDrive()) { toast(driveLockReason(), 'warn'); return; }
    pad.setPointerCapture(e.pointerId);
    DRV.held = true; pad.classList.add('held');
    move(e); startSending();
  });
  pad.addEventListener('pointermove', (e) => { if (DRV.held && !DRV.keys.size) move(e); });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((t) => pad.addEventListener(t, () => { if (!DRV.keys.size) releaseDrive(); }));

  const onDrivePage = () => location.hash === '#drive';
  window.addEventListener('keydown', (e) => {
    if (!onDrivePage() || e.target.closest?.('input, textarea, select')) return;
    if (e.code === 'Space' || e.code === 'Escape') { e.preventDefault(); releaseDrive(); return; }
    const k = KEYMAP[e.code]; if (!k) return;
    e.preventDefault();
    if (!canDrive()) { if (!e.repeat) toast(driveLockReason(), 'warn'); return; }
    DRV.keys.add(k); keysToTarget();
  });
  window.addEventListener('keyup', (e) => {
    const k = KEYMAP[e.code]; if (!k) return;
    DRV.keys.delete(k);
    if (DRV.keys.size) keysToTarget(); else releaseDrive();
  });
  window.addEventListener('blur', releaseDrive);
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseDrive(); });
}

function driveLockReason() {
  const st = DRV.state;
  if (!ctlOnline()) return 'The robot\'s drive controller is offline.';
  if (st.mode === 'STOP') return 'Emergency stop is on. Choose Manual to drive.';
  if (st.mode !== 'MANUAL') return 'Switch to Manual first.';
  if (!st.manualCapable) return 'Manual driving needs Arduino firmware 2.1.';
  if (st.arduino !== 'online') return 'The Arduino is not connected.';
  return 'Driving is not available right now.';
}

function keysToTarget() {
  const k = DRV.keys;
  const x = (k.has('r') ? 1 : 0) - (k.has('l') ? 1 : 0);
  const y = (k.has('f') ? 1 : 0) - (k.has('b') ? 1 : 0);
  const m = Math.hypot(x, y) || 1;
  setKnob(x / m, -y / m);
  DRV.held = true; el('pad').classList.add('held');
  setTarget(x, y);
  startSending();
}

function setKnob(dx, dy) { el('knob').style.transform = `translate(${dx * 140}%, ${dy * 140}%)`; }

// x: right +, y: forward +  →  differential wheel PWM
function setTarget(x, y) {
  DRV.x = x; DRV.y = y;
  const sp = Math.min(DRV.speed, DRV.state?.maxPwm || 200);
  let v = y * sp, w;
  if (Math.abs(y) < 0.2) { v = 0; w = x * sp * 0.75; }    // near the middle line: turn on the spot
  else w = x * sp * 0.55;
  const cl = (n) => Math.max(-200, Math.min(200, Math.round(n)));
  DRV.l = cl(v + w); DRV.r = cl(v - w);
  renderWheels();
}

function startSending() {
  if (DRV.timer) return;
  if (!DRV.disc) {   // if this browser vanishes mid-drive, Firebase zeroes the command for us
    DRV.disc = true;
    onDisconnect(ref(rtdb, `${ctlPath}/drive`)).set({ l: 0, r: 0, seq: 0, by: S.user.uid, name: myName(), at: serverTimestamp() }).catch(() => { DRV.disc = false; });
  }
  sendDrive();
  DRV.timer = setInterval(sendDrive, 150);
}

function sendDrive() {
  if (!canDrive()) { releaseDrive(); return; }
  DRV.seq += 1;
  set(ref(rtdb, `${ctlPath}/drive`), { l: DRV.l, r: DRV.r, seq: DRV.seq, by: S.user.uid, name: myName(), at: serverTimestamp() }).catch(() => {});
}

function releaseDrive() {
  const wasActive = !!DRV.timer || DRV.held;
  DRV.held = false; DRV.keys.clear();
  if (DRV.timer) { clearInterval(DRV.timer); DRV.timer = null; }
  DRV.x = DRV.y = DRV.l = DRV.r = 0;
  const pad = el('pad'); if (pad) { pad.classList.remove('held'); setKnob(0, 0); }
  if (wasActive && S.user) {
    DRV.seq += 1;
    set(ref(rtdb, `${ctlPath}/drive`), { l: 0, r: 0, seq: DRV.seq, by: S.user.uid, name: myName(), at: serverTimestamp() }).catch(() => {});
  }
  renderWheels();
}

async function requestMode(mode) {
  if (mode !== 'MANUAL') releaseDrive();
  try {
    await set(ref(rtdb, `${ctlPath}/request`), { mode, by: S.user.uid, name: myName(), id: crypto.randomUUID(), at: serverTimestamp() });
    toast(mode === 'STOP' ? 'Emergency stop sent.' : mode === 'AUTO' ? 'Auto (line following) requested.' : 'Manual driving requested.', mode === 'STOP' ? 'crit' : 'info');
  } catch (e) {
    toast(`Could not send: ${e.message}`, 'crit');
  }
}

function renderWheels() {
  const st = DRV.state;
  const ap = st?.applied || { l: 0, r: 0 };
  const live = ctlOnline() && st?.mode === 'MANUAL';
  [['L', live ? ap.l : 0], ['R', live ? ap.r : 0]].forEach(([k, v]) => {
    const bar = el(`w${k}`); const pct = Math.min(1, Math.abs(v) / 200) * 50;
    bar.style.width = `${pct}%`; bar.style.left = v >= 0 ? '50%' : `${50 - pct}%`;
    bar.classList.toggle('rev', v < 0);
    el(`w${k}v`).textContent = v;
  });
}

function renderDrive() {
  const st = DRV.state;
  const on = ctlOnline();
  const mode = on ? st.mode : null;
  const want = DRV.request?.mode;

  const cp = el('drvCtlPill');
  cp.className = `pill ${on ? 'ok' : 'crit'}`; cp.textContent = on ? 'Controller online' : 'Controller offline';
  const mp = el('drvModePill');
  mp.className = `pill ${mode === 'STOP' ? 'crit' : mode === 'MANUAL' ? 'warn' : mode === 'AUTO' ? 'info' : ''}`;
  mp.textContent = mode === 'STOP' ? 'EMERGENCY STOP' : mode || '—';
  el('modeAuto').classList.toggle('on', mode === 'AUTO');
  el('modeManual').classList.toggle('on', mode === 'MANUAL');
  el('modeManual').disabled = on && !st.manualCapable;
  el('estopBtn').classList.toggle('latched', mode === 'STOP');

  let note;
  if (!on) note = st ? `The drive controller on the Pi stopped reporting ${fmtAgo(serverNow() - (st.updatedAt || 0))} ago. Requests are saved and applied when it reconnects.` : 'Waiting for the robot\'s drive controller (medicart-control on the Pi)…';
  else if (want && want !== st.mode) note = `Requested ${want}… waiting for the robot.`;
  else if (st.mode === 'AUTO') note = 'The cart follows the line by itself. medicart.py stops it for people.';
  else if (st.mode === 'MANUAL') note = st.manualCapable ? `Manual: drive with the pad. Set by ${st.modeBy || '—'}.` : 'Manual needs Arduino firmware 2.1.';
  else note = `Stopped by ${st.modeBy || '—'}. Choose Auto or Manual to release.`;
  if (on && !st.manualCapable) note += ' Manual driving needs Arduino firmware 2.1 on the Mega.';
  el('drvModeNote').textContent = note;

  const drivable = canDrive();
  const pad = el('pad');
  pad.classList.toggle('locked', !drivable);
  const pp = el('drvPadPill');
  pp.className = `pill ${drivable ? (DRV.held ? 'warn' : 'ok') : ''}`;
  pp.textContent = drivable ? (DRV.held ? 'Driving' : 'Ready') : 'Locked';
  if (!drivable && (DRV.held || DRV.timer)) releaseDrive();

  const alert = el('drvAlert');
  let msg = '', crit = false;
  if (on && st.mode === 'STOP') { msg = 'Emergency stop is latched. The wheels stay still until you choose Auto or Manual.'; crit = true; }
  else if (on && st.blocked) { msg = 'Person ahead: forward is blocked. Turn on the spot or reverse.'; crit = true; }
  else if (on && st.mode === 'MANUAL' && st.vision === 'offline') msg = `Vision (person detection) is offline. Speed is limited to ${st.slowPwm} PWM. Watch the path yourself.`;
  else if (on && st.mode === 'MANUAL' && st.driver && st.driver !== myName() && (st.applied?.l || st.applied?.r)) msg = `${st.driver} is driving too. The last command wins.`;
  alert.hidden = !msg; alert.textContent = msg; alert.classList.toggle('crit', crit);

  el('drvArd').textContent = !st ? '—' : st.arduino === 'online' ? `Online${st.arduinoPort ? ` · ${st.arduinoPort.replace('/dev/', '')}` : ''}` : 'Not connected';
  el('drvFw').textContent = st?.firmware || '—';
  el('drvVision').textContent = !st ? '—' : { clear: 'Running · path clear', blocked: 'Running · person in stop zone', offline: 'Not running' }[st.vision] || st.vision;
  const line = (on && st.line) || '';
  el('drvLine').querySelectorAll('i').forEach((i, n) => i.classList.toggle('on', line[n] === '1'));
  el('drvLine').title = line ? `${line}${st.lineLost ? ' · line lost' : ''}` : 'No data';
  el('drvDriver').textContent = on && st.mode === 'MANUAL' ? (st.driver || 'Nobody') : on && st.mode === 'AUTO' ? 'medicart.py (auto)' : '—';
  el('drvSeen').textContent = st?.updatedAt ? fmtAgo(serverNow() - st.updatedAt) : '—';
  if (on) el('spd').max = st.maxPwm || 200;

  const evs = (st?.events || []).slice().reverse();
  el('drvEvents').innerHTML = evs.length
    ? evs.map((e) => `<li><time>${new Date(e.t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</time><span>${escapeHTML(e.text)}</span></li>`).join('')
    : '<li class="empty">No events yet.</li>';

  // rail + nav badge
  el('rsDrive').className = `dot ${!on ? 'crit' : mode === 'STOP' ? 'crit' : mode === 'MANUAL' ? 'warn' : 'ok'}${on ? ' live' : ''}`;
  el('rsDriveText').textContent = !on ? 'Drive controller offline' : mode === 'STOP' ? 'Drive: EMERGENCY STOP' : `Drive: ${mode === 'AUTO' ? 'Auto' : 'Manual'}`;
  const nb = el('navDrive');
  nb.hidden = !(on && (mode === 'MANUAL' || mode === 'STOP'));
  nb.textContent = mode === 'STOP' ? 'STOP' : 'MANUAL';
  renderWheels();
}

// ─── Charts ─────────────────────────────────────────────────────
const charts = {};
function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
function chartTheme() {
  return { grid: cssVar('--line-2') || '#E5EAF1', tick: cssVar('--text-3') || '#75859A' };
}
function makeChart(canvasId, colorVar, yHints = {}) {
  const t = chartTheme();
  const color = cssVar(colorVar);
  return new window.Chart(el(canvasId), {
    type: 'line',
    data: { labels: [], datasets: [{ data: [], borderColor: color, backgroundColor: `${color}1A`, fill: true, borderWidth: 2, tension: 0.32, pointRadius: 0, pointHitRadius: 8, spanGaps: false }] },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: 'index', intersect: false },
      plugins: { legend: { display: false }, tooltip: { displayColors: false } },
      scales: {
        x: { ticks: { maxTicksLimit: 6, color: t.tick, font: { family: 'DM Mono', size: 10 } }, grid: { color: t.grid }, border: { display: false } },
        y: { ...yHints, ticks: { color: t.tick, font: { family: 'DM Mono', size: 10 } }, grid: { color: t.grid }, border: { display: false } },
      },
    },
  });
}
function initCharts() {
  if (!window.Chart) { console.warn('Chart.js did not load'); return; }
  charts.hr = makeChart('cHR', '--hr', { suggestedMin: 50, suggestedMax: 120 });
  charts.sp = makeChart('cSP', '--spo2', { suggestedMin: 88, suggestedMax: 100 });
  charts.ir = makeChart('cIR', '--ir');
  charts.red = makeChart('cRED', '--red');
  const vars = { hr: '--hr', sp: '--spo2', ir: '--ir', red: '--red' };
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    const t = chartTheme();
    for (const [k, c] of Object.entries(charts)) {
      const color = cssVar(vars[k]);
      c.data.datasets[0].borderColor = color; c.data.datasets[0].backgroundColor = `${color}1A`;
      for (const ax of ['x', 'y']) { c.options.scales[ax].grid.color = t.grid; c.options.scales[ax].ticks.color = t.tick; }
      c.update('none');
    }
  });
  el('winSeg').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    S.chartWin = Number(b.dataset.win);
    el('winSeg').querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
    redrawCharts();
  });
}
function pushSeries(d) {
  const t = Date.now();
  const valid = d.fingerDetected;
  S.series.hr.push({ t, v: valid && d.heartRate > 0 ? d.heartRate : null });
  S.series.sp.push({ t, v: valid && d.spo2 > 0 ? d.spo2 : null });
  S.series.ir.push({ t, v: d.ir || null });
  S.series.red.push({ t, v: d.red || null });
  const keep = t - 1800 * 1000;
  for (const k of Object.keys(S.series)) while (S.series[k].length && S.series[k][0].t < keep) S.series[k].shift();
  redrawCharts();
}
function redrawCharts() {
  if (!charts.hr) return;
  const from = Date.now() - S.chartWin * 1000;
  const label = (t) => new Date(t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  for (const k of Object.keys(charts)) {
    const pts = S.series[k].filter((p) => p.t >= from);
    charts[k].data.labels = pts.map((p) => label(p.t));
    charts[k].data.datasets[0].data = pts.map((p) => p.v);
    charts[k].update('none');
  }
  const stat = (k) => {
    const v = S.series[k].filter((p) => p.t >= from && p.v != null).map((p) => p.v);
    if (!v.length) return ['--', '--', '--'];
    return [Math.min(...v), Math.round(v.reduce((a, b) => a + b, 0) / v.length), Math.max(...v)];
  };
  const [a, b, c] = stat('hr'); el('sHRmin').textContent = a; el('sHRavg').textContent = b; el('sHRmax').textContent = c;
  const [d, e, f] = stat('sp'); el('sSPmin').textContent = d; el('sSPavg').textContent = e; el('sSPmax').textContent = f;
  const lastV = (k) => { const s = S.series[k]; const v = s.length ? s[s.length - 1].v : null; return v ? v.toLocaleString() : '--'; };
  el('sIR').textContent = lastV('ir'); el('sRED').textContent = lastV('red');
}

// ─── Data stream page ───────────────────────────────────────────
function initStream() {
  el('rawPause').addEventListener('click', () => { S.rawPaused = !S.rawPaused; el('rawPause').textContent = S.rawPaused ? 'Resume' : 'Pause'; });
  el('rawClear').addEventListener('click', () => { S.raw = []; renderRaw(); });
  el('rawCsv').addEventListener('click', () => {
    if (!S.raw.length) return toast('Nothing to export yet.');
    download(`medicart-stream-${stamp()}.csv`, toCSV(S.raw), 'text/csv');
  });
  el('copyJson').addEventListener('click', async () => {
    if (!S.lastMsg) return;
    try { await navigator.clipboard.writeText(S.lastMsg.text); toast('JSON copied.', 'ok'); } catch { toast('Copy blocked by the browser.', 'warn'); }
  });
}
function pushRaw(d) {
  if (S.rawPaused) return;
  S.raw.unshift({
    time: new Date(d.at).toLocaleTimeString(), hr: d.heartRate, spo2: d.spo2, ir: d.ir, red: d.red,
    finger: d.fingerDetected ? 'yes' : 'no', state: d.state, samples: d.samplesCollected,
  });
  if (S.raw.length > 300) S.raw.length = 300;
  renderRaw();
}
function renderRaw() {
  el('rawCount').textContent = `${S.raw.length} packets (latest 300 kept)`;
  el('rawBody').innerHTML = S.raw.slice(0, 80).map((r) =>
    `<tr><td>${r.time}</td><td>${r.hr}</td><td>${r.spo2}</td><td>${r.ir.toLocaleString()}</td><td>${r.red.toLocaleString()}</td><td>${r.finger}</td><td>${escapeHTML(r.state)}</td><td>${r.samples}</td></tr>`).join('');
}
function renderInspector() {
  if (!S.lastMsg || !el('page-stream').classList.contains('on')) return;
  el('inspTopic').textContent = S.lastMsg.topic;
  el('inspTime').textContent = new Date(S.lastMsg.at).toLocaleTimeString();
  let pretty;
  try { pretty = JSON.stringify(JSON.parse(S.lastMsg.text), null, 2); } catch { el('inspPayload').textContent = S.lastMsg.text; return; }
  el('inspPayload').innerHTML = escapeHTML(pretty).replace(
    /(&quot;(?:\\.|[^&]|&(?!quot;))*?&quot;)(\s*:)?|\b(true|false|null)\b|(-?\d+(?:\.\d+)?)/g,
    (m, str, colon, bool, num) => {
      if (str) return colon ? `<span class="k">${str}</span>${colon}` : `<span class="s">${str}</span>`;
      if (bool) return `<span class="b">${bool}</span>`;
      return `<span class="n">${num}</span>`;
    });
}

// ─── System page ────────────────────────────────────────────────
function initSystem() {
  el('profileForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const name = el('profileName').value.trim();
    if (!name) return toast('Enter a name first.', 'warn');
    try {
      await updateProfile(auth.currentUser, { displayName: name });
      renderUser();
      toast('Name saved. Patients will see it when you call.', 'ok');
    } catch (err) { toast(`Could not save: ${err.message}`, 'crit'); }
  });
  el('mqttToggle').addEventListener('click', () => (S.mqtt ? stopMqtt() : startMqtt()));
}

// ─── Records: consultations + sensor history ────────────────────
function initRecords() {
  document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('on', x === t));
    el('tab-consults').hidden = t.dataset.tab !== 'consults';
    el('tab-history').hidden = t.dataset.tab !== 'history';
    if (t.dataset.tab === 'history' && !S.history.length) loadHistory();
  }));
  el('consultReload').addEventListener('click', loadConsultations);
  el('consultCsv').addEventListener('click', () => exportConsults('csv'));
  el('consultJson').addEventListener('click', () => exportConsults('json'));
  el('histSeg').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    S.histN = Number(b.dataset.n);
    el('histSeg').querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
    loadHistory();
  });
  el('histCsv').addEventListener('click', () => exportHistory('csv'));
  el('histJson').addEventListener('click', () => exportHistory('json'));
  let armed = null;
  el('histClear').addEventListener('click', async (e) => {
    const b = e.currentTarget;
    if (!armed) {
      b.textContent = 'Tap again to delete all'; b.classList.add('btn-danger');
      armed = setTimeout(() => { armed = null; b.textContent = 'Clear history'; b.classList.remove('btn-danger'); }, 4000);
      return;
    }
    clearTimeout(armed); armed = null; b.textContent = 'Deleting…'; b.disabled = true;
    try {
      let total = 0;
      for (;;) {
        const snap = await getDocs(query(collection(db, 'sensor_history'), limit(400)));
        if (snap.empty) break;
        const batch = writeBatch(db);
        snap.forEach((d) => batch.delete(d.ref));
        await batch.commit();
        total += snap.size;
      }
      toast(`Deleted ${total} readings.`, 'ok');
      loadHistory();
    } catch (err) { toast(`Delete failed: ${err.message}`, 'crit'); }
    b.disabled = false; b.textContent = 'Clear history'; b.classList.remove('btn-danger');
  });
}

async function loadConsultations() {
  try {
    const snap = await getDocs(query(collection(db, 'consultations'), orderBy('startedAt', 'desc'), limit(200)));
    S.consults = [];
    snap.forEach((d) => S.consults.push({ id: d.id, ...d.data() }));
  } catch (err) {
    el('consultBody').innerHTML = `<tr><td colspan="7" class="empty">Could not load: ${escapeHTML(err.message)}</td></tr>`;
    return;
  }
  el('consultCount').textContent = `${S.consults.length} consultation${S.consults.length === 1 ? '' : 's'}`;
  el('consultBody').innerHTML = S.consults.length ? S.consults.map((c) => {
    const [label, tone] = OUTCOME[c.outcome] || [c.outcome || '—', 'neutral'];
    const start = toDate(c.startedAt);
    return `<tr><td>${start ? start.toLocaleString() : '—'}</td><td>${escapeHTML(c.doctorName || '—')}</td><td class="mono">${c.durationSec ? fmtClock(c.durationSec) : '—'}</td>
      <td><span class="pill ${tone === 'neutral' ? '' : tone}">${label}</span></td><td>${c.vitals?.hr || '—'}</td><td>${c.vitals?.spo2 || '—'}</td>
      <td class="notes-cell">${escapeHTML(c.notes || '')}</td></tr>`;
  }).join('') : '<tr><td colspan="7" class="empty">No consultations yet.</td></tr>';

  el('recentList').innerHTML = S.consults.length ? S.consults.slice(0, 5).map((c) => {
    const [label, tone] = OUTCOME[c.outcome] || [c.outcome || '—', 'neutral'];
    const start = toDate(c.startedAt);
    return `<li><b>${escapeHTML(c.doctorName || 'Clinician')}</b><span class="pill r-out ${tone === 'neutral' ? '' : tone}">${label}</span>
      <span>${start ? start.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) : '—'}${c.durationSec ? ` · ${fmtClock(c.durationSec)}` : ''}</span></li>`;
  }).join('') : '<li class="empty">No consultations yet.</li>';
}

function exportConsults(fmt) {
  if (!S.consults.length) return toast('No consultations to export.');
  const rows = S.consults.map((c) => ({
    started: toDate(c.startedAt)?.toISOString() || '', ended: toDate(c.endedAt)?.toISOString() || '',
    clinician: c.doctorName || '', clinician_email: c.doctorEmail || '', duration_sec: c.durationSec || 0,
    outcome: c.outcome || '', hr: c.vitals?.hr || '', spo2: c.vitals?.spo2 || '', notes: c.notes || '', cart: c.cartId || '',
  }));
  if (fmt === 'csv') download(`medicart-consultations-${stamp()}.csv`, toCSV(rows), 'text/csv');
  else download(`medicart-consultations-${stamp()}.json`, JSON.stringify(rows, null, 2), 'application/json');
}

async function loadHistory() {
  el('histBody').innerHTML = '<tr><td colspan="7" class="empty">Loading…</td></tr>';
  try {
    const col = collection(db, 'sensor_history');
    const q = S.histN ? query(col, orderBy('timestamp', 'desc'), limit(S.histN)) : query(col, orderBy('timestamp', 'desc'));
    const snap = await getDocs(q);
    S.history = [];
    snap.forEach((d) => S.history.push({ id: d.id, ...d.data() }));
  } catch (err) {
    el('histBody').innerHTML = `<tr><td colspan="7" class="empty">Could not load: ${escapeHTML(err.message)}</td></tr>`;
    return;
  }
  el('histCount').textContent = `${S.history.length} readings`;
  el('histBody').innerHTML = S.history.length ? S.history.map((r) => {
    const t = toDate(r.timestamp);
    return `<tr><td>${t ? t.toLocaleString() : '—'}</td><td>${r.heartRate || 0}</td><td>${r.spo2 || 0}</td><td>${(r.ir || 0).toLocaleString()}</td>
      <td>${(r.red || 0).toLocaleString()}</td><td>${r.fingerDetected ? 'yes' : 'no'}</td><td>${escapeHTML(stateLabel(r.state))}</td></tr>`;
  }).join('') : '<tr><td colspan="7" class="empty">No readings stored yet. The cart screen logs one every 5 s while a finger is on the sensor.</td></tr>';
}

function exportHistory(fmt) {
  if (!S.history.length) return toast('Load some readings first.');
  const rows = S.history.map((r) => ({
    time: toDate(r.timestamp)?.toISOString() || '', heartRate: r.heartRate || 0, spo2: r.spo2 || 0,
    ir: r.ir || 0, red: r.red || 0, finger: !!r.fingerDetected, state: r.state || '', device: r.deviceId || '',
  }));
  if (fmt === 'csv') download(`medicart-history-${stamp()}.csv`, toCSV(rows), 'text/csv');
  else download(`medicart-history-${stamp()}.json`, JSON.stringify(rows, null, 2), 'application/json');
}

const stamp = () => new Date().toISOString().slice(0, 16).replace(/[:T]/g, '-');

// ════════════════════════════════════════════════════════════════
//  WebRTC — doctor is the caller. Signalling lives at /calls/{cart}
//  { callId, status: ringing|active|ended|declined, offer, answer,
//    offerCandidates/*, answerCandidates/*, prompt, doctor }
// ════════════════════════════════════════════════════════════════
const callPath = `calls/${CART_ID}`;

function initConsult() {
  el('callBtn').addEventListener('click', startCall);
  el('cancelBtn').addEventListener('click', () => hangup('cancelled'));
  el('endBtn').addEventListener('click', () => hangup('completed'));
  el('micBtn').addEventListener('click', () => toggleTrack('audio', 'micBtn'));
  el('camBtn').addEventListener('click', () => toggleTrack('video', 'camBtn'));
  el('spkBtn').addEventListener('click', () => {
    const v = el('remoteVideo'); v.muted = !v.muted;
    el('spkBtn').classList.toggle('off', v.muted);
  });
  el('fsBtn').addEventListener('click', () => {
    if (document.fullscreenElement) document.exitFullscreen();
    else el('stage').requestFullscreen?.().catch(() => {});
  });
  el('tapSound').addEventListener('click', () => {
    el('remoteVideo').play().then(() => { el('tapSound').hidden = true; }).catch(() => {});
  });
  el('prompts').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-prompt]');
    if (b) sendPrompt({ key: b.dataset.prompt });
  });
  el('promptForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = el('promptText').value.trim();
    if (!text) return;
    sendPrompt({ text });
    el('promptText').value = '';
  });
  window.addEventListener('beforeunload', () => {
    if (S.call) update(ref(rtdb, callPath), { status: 'ended', endedBy: 'doctor' });
  });
}

function renderCallAvailability() {
  if (S.call) return;
  const on = cartOnline();
  const busy = on && S.cart?.inCall;
  el('callBtn').disabled = !on || busy;
  el('stageTitle').textContent = !on ? `${CART_LABEL} is offline` : busy ? `${CART_LABEL} is in another call` : `${CART_LABEL} is ready`;
  el('stageSub').textContent = !on
    ? 'The cart screen must be powered on and signed in before you can call.'
    : busy ? `Currently with ${S.cart.callDoctor || 'another clinician'}. Try again when they finish.`
    : `Camera: ${S.cart.camera || 'not detected'} · Microphone: ${S.cart.mic || 'not detected'}`;
}

function setStage(state) {
  const stage = el('stage');
  stage.dataset.state = state;
  el('stageIdle').hidden = state !== 'idle';
  el('stageRinging').hidden = state !== 'ringing';
  const inCall = state === 'connecting' || state === 'live';
  el('stageTop').hidden = !inCall;
  el('controls').hidden = state === 'idle';
  el('pip').hidden = state === 'idle';
  el('navLive').hidden = !inCall;
  document.querySelectorAll('#prompts button, #promptText, #promptSend').forEach((b) => { b.disabled = !inCall; });
}

async function getDoctorMedia() {
  const audio = { echoCancellation: true, noiseSuppression: true, autoGainControl: true };
  try {
    return await navigator.mediaDevices.getUserMedia({ video: DOCTOR_VIDEO, audio });
  } catch (err) {
    if (err.name === 'NotAllowedError') throw err;
    // no camera on this computer — continue audio-only
    const s = await navigator.mediaDevices.getUserMedia({ audio });
    toast('No camera found — calling with audio only.', 'warn');
    return s;
  }
}

async function startCall() {
  if (S.call) return;
  if (!cartOnline()) return toast('The cart is offline.', 'warn');
  if (S.cart?.inCall) return toast('The cart is already in a call.', 'warn');

  el('callStatus').textContent = 'Requesting your camera and microphone…';
  let local;
  try {
    local = await getDoctorMedia();
  } catch (err) {
    el('callStatus').textContent = 'Camera/microphone blocked. Allow access in the address bar, then try again.';
    toast('Allow camera and microphone to start a call.', 'crit');
    return;
  }

  const callId = crypto.randomUUID();
  const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
  const C = S.call = {
    id: callId, pc, local, startedAt: Date.now(), connectedAt: null, outcome: null,
    unsubs: [], pendingLocal: [], pendingRemote: [], nodeReady: false, disc: null,
    ringTimer: null, statsTimer: null, dropTimer: null, finished: false,
  };

  el('localVideo').srcObject = local;
  local.getTracks().forEach((t) => pc.addTrack(t, local));
  if (!local.getVideoTracks().length) pc.addTransceiver('video', { direction: 'recvonly' });

  pc.ontrack = (e) => {
    const v = el('remoteVideo');
    if (v.srcObject !== e.streams[0]) {
      v.srcObject = e.streams[0] || new MediaStream([e.track]);
      v.play().catch(() => { el('tapSound').hidden = false; });
    }
  };
  pc.onicecandidate = (e) => {
    if (!e.candidate) return;
    const c = packCandidate(e.candidate, callId);
    if (C.nodeReady) push(ref(rtdb, `${callPath}/offerCandidates`), c);
    else C.pendingLocal.push(c);
  };
  pc.onconnectionstatechange = () => {
    const st = pc.connectionState;
    if (st === 'connected') {
      clearTimeout(C.dropTimer); el('stageReconnect').hidden = true;
      if (!C.connectedAt) {
        C.connectedAt = Date.now();
        setStage('live');
        el('callStatus').textContent = `In consultation with the patient at ${CART_LABEL}.`;
        C.statsTimer = setInterval(updateNetStats, 2000);
      }
    } else if (st === 'disconnected') {
      el('stageReconnect').hidden = false;
      C.dropTimer = setTimeout(() => { if (pc.connectionState !== 'connected') hangup('cart_lost'); }, 15000);
    } else if (st === 'failed') {
      toast('Could not establish media between you and the cart. A TURN server may be needed — see README.', 'crit', 8000);
      hangup('failed');
    }
  };

  setStage('ringing');
  el('callStatus').textContent = `Calling ${CART_LABEL}…`;

  try {
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    const callRef = ref(rtdb, callPath);
    await set(callRef, {
      callId,
      status: 'ringing',
      offer: { type: offer.type, sdp: offer.sdp },
      doctor: { uid: S.user.uid, name: doctorName(), email: S.user.email || '' },
      createdAt: serverTimestamp(),
    });
    C.nodeReady = true;
    C.pendingLocal.forEach((c) => push(ref(rtdb, `${callPath}/offerCandidates`), c));
    C.pendingLocal = [];

    C.disc = onDisconnect(callRef);
    await C.disc.update({ status: 'ended', endedBy: 'doctor' });

    C.unsubs.push(onValue(callRef, (snap) => onCallNode(C, snap.val())));
    C.unsubs.push(onChildAdded(ref(rtdb, `${callPath}/answerCandidates`), (snap) => {
      const v = snap.val();
      if (!v || v.callId !== callId) return;
      const init = unpackCandidate(v);
      if (pc.remoteDescription) pc.addIceCandidate(init).catch((e) => console.warn('ice', e));
      else C.pendingRemote.push(init);
    }));

    C.ringTimer = setTimeout(() => { if (!C.answered) hangup('no_answer'); }, RING_TIMEOUT_MS);
  } catch (err) {
    console.error(err);
    toast(`Call failed to start: ${err.message}`, 'crit');
    hangup('failed');
  }
}

async function onCallNode(C, d) {
  if (C.finished) return;
  if (!d || d.callId !== C.id) { finish(C, C.answered ? 'cart_lost' : 'cancelled'); return; }
  if (d.answer && !C.answered) {
    C.answered = true;
    clearTimeout(C.ringTimer);
    setStage('connecting');
    el('callStatus').textContent = 'Patient answered — connecting media…';
    try {
      await C.pc.setRemoteDescription(d.answer);
      C.pendingRemote.forEach((c) => C.pc.addIceCandidate(c).catch(() => {}));
      C.pendingRemote = [];
    } catch (err) {
      console.error(err);
      hangup('failed');
      return;
    }
  }
  if (d.status === 'declined') { toast('The call was declined at the cart.', 'warn'); finish(C, 'declined'); }
  else if (d.status === 'ended' && d.endedBy === 'cart') {
    if (d.endReason === 'cart_offline') { toast('The cart lost its connection.', 'warn'); finish(C, 'cart_lost'); }
    else if (d.endReason === 'failed') finish(C, 'failed');
    else { toast('The call was ended at the cart.'); finish(C, 'ended_by_cart'); }
  }
}

async function hangup(outcome) {
  const C = S.call;
  if (!C || C.finished) return;
  try {
    await update(ref(rtdb, callPath), { status: 'ended', endedBy: 'doctor', endedAt: serverTimestamp() });
  } catch (err) { console.warn('could not mark call ended', err); }
  finish(C, outcome);
}

function finish(C, outcome) {
  if (C.finished) return;
  C.finished = true;
  C.outcome = outcome;
  clearTimeout(C.ringTimer); clearTimeout(C.dropTimer); clearInterval(C.statsTimer);
  C.unsubs.forEach((u) => u());
  C.disc?.cancel().catch(() => {});
  try { C.pc.getSenders().forEach((s) => s.track?.stop()); C.pc.close(); } catch { /* already closed */ }
  C.local.getTracks().forEach((t) => t.stop());
  el('remoteVideo').srcObject = null;
  el('localVideo').srcObject = null;
  el('tapSound').hidden = true;
  el('stageReconnect').hidden = true;
  el('micBtn').classList.remove('off'); el('camBtn').classList.remove('off'); el('spkBtn').classList.remove('off');
  el('remoteVideo').muted = false;
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  S.call = null;
  setStage('idle');
  const [label] = OUTCOME[outcome] || [outcome];
  el('callStatus').textContent = `Last call: ${label}${C.connectedAt ? ` · ${fmtClock((Date.now() - C.connectedAt) / 1000)}` : ''}`;
  el('netStats').textContent = '—';
  el('callTimer').textContent = '00:00';
  saveConsultation(C);
  renderCallAvailability();
}

async function saveConsultation(C) {
  const notes = el('notes').value.trim();
  const v = S.vit && sensorOnline() ? S.vit : null;
  const record = {
    cartId: CART_ID,
    callId: C.id,
    doctorUid: S.user.uid,
    doctorName: doctorName(),
    doctorEmail: S.user.email || '',
    startedAt: Timestamp.fromDate(new Date(C.startedAt)),
    connectedAt: C.connectedAt ? Timestamp.fromDate(new Date(C.connectedAt)) : null,
    endedAt: fsTimestamp(),
    durationSec: C.connectedAt ? Math.round((Date.now() - C.connectedAt) / 1000) : 0,
    outcome: C.outcome,
    notes,
    vitals: v ? { hr: v.heartRate || null, spo2: v.spo2 || null, finger: v.fingerDetected, state: v.state } : null,
  };
  try {
    await addDoc(collection(db, 'consultations'), record);
    if (notes) el('notes').value = '';
    if (C.connectedAt) toast('Consultation saved to Records.', 'ok');
    loadConsultations();
  } catch (err) {
    toast(`Could not save the consultation: ${err.message}. Your notes are still in the box.`, 'crit', 8000);
  }
}

function toggleTrack(kind, btnId) {
  const C = S.call; if (!C) return;
  const tracks = kind === 'audio' ? C.local.getAudioTracks() : C.local.getVideoTracks();
  if (!tracks.length) return toast(kind === 'audio' ? 'No microphone.' : 'No camera.', 'warn');
  const enable = !tracks[0].enabled;
  tracks.forEach((t) => { t.enabled = enable; });
  el(btnId).classList.toggle('off', !enable);
}

async function sendPrompt(p) {
  if (!S.call?.connectedAt) return;
  const id = Math.random().toString(36).slice(2, 10);
  const payload = p.key ? { id, key: p.key, en: PROMPTS[p.key].en, bn: PROMPTS[p.key].bn } : { id, en: p.text, bn: '' };
  try {
    await set(ref(rtdb, `${callPath}/prompt`), { ...payload, at: serverTimestamp() });
    toast(`Shown on cart: “${payload.en}”`, 'ok', 2600);
  } catch (err) { toast(`Could not send: ${err.message}`, 'crit'); }
}

async function updateNetStats() {
  const C = S.call; if (!C) return;
  try {
    const s = await readStats(C.pc);
    if (!s) return;
    const parts = [];
    if (s.rtt != null) parts.push(`${s.rtt} ms`);
    if (s.fps != null) parts.push(`${Math.round(s.fps)} fps`);
    if (s.relay) parts.push('relay');
    el('netStats').textContent = parts.join(' · ') || '—';
  } catch { /* stats unavailable */ }
}
}

if (window.MC_FB) main();
else window.addEventListener('mc-ready', main, { once: true });
})();
