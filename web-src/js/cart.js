// ════════════════════════════════════════════════════════════════
//  MediCart — cart screen (runs on the Raspberry Pi in kiosk mode)
//  Presence · answers doctor calls (WebRTC callee) · shows vitals ·
//  logs sensor readings to Firebase (single writer for the cart).
// ════════════════════════════════════════════════════════════════
(function () {
// Runs once the Firebase loader at the end of the page has finished.
function main() {
const {
  auth, rtdb, db, onAuthStateChanged, signInWithEmailAndPassword, signOut, ref, set, update, push,
  onValue, onChildAdded, onDisconnect, serverTimestamp, collection, addDoc, fsTimestamp,
} = window.MC_FB;
const {
  connectMqtt, serverNow, packCandidate, unpackCandidate, el, toast, fmtClock, authErrorText,
} = window.MC;
const {
  APP_VERSION, CART_ID, CART_LABEL, ICE_SERVERS, CART_VIDEO, CART_CAMERA_HINT,
  DEFAULT_AUTO_ANSWER_S, SENSOR_STALE_MS, HISTORY_INTERVAL_MS,
} = window.MC_CONFIG;

const PREFS_KEY = 'medicart.cart.prefs';
const callPath = `calls/${CART_ID}`;
const presencePath = `carts/${CART_ID}`;

const S = {
  user: null,
  started: false,
  connected: false,
  local: null,
  camLabel: null,
  micLabel: null,
  mediaError: null,
  vit: null,
  lastSensorAt: 0,
  lastHistoryAt: 0,
  logged: 0,
  ring: null,        // { callId, data, timer, countdown, audio }
  call: null,        // active call
  handled: new Set(),
  lastPromptId: null,
  wakeLock: null,
};

const prefs = loadPrefs();

// ─── Voice announcements (Bangla) ───────────────────────────────
// The team's recorded announcements. One plays at a time; the same one is never queued twice.
const SOUNDS = {
  '0001': 'assets/sounds/0001.mp3', // অনুগ্রহ করে নির্ধারিত স্থান থেকে সরে দাঁড়ান — a person blocks the cart's path
  '0002': 'assets/sounds/0002.mp3', // নিরবচ্ছিন্ন সেবা নিশ্চিত করতে অনুগ্রহপূর্বক নির্ধারিত স্থানটি খালি রাখুন — still blocked
  '0004': 'assets/sounds/0004.mp3', // এখন আপনার রক্তের নমুনা সংগ্রহ করা হবে… তর্জনী আঙুলটি… স্থাপন করুন — the cart has arrived (stopped)
  '0005': 'assets/sounds/0005.mp3', // নমুনা সংগ্রহ সম্পন্ন না হওয়া পর্যন্ত অনুগ্রহ করে আঙুলটি স্থির রাখুন
  '0006': 'assets/sounds/0006.mp3', // আপনার সকল নমুনা সফলভাবে সংগ্রহ করা হয়েছে, সহযোগিতার জন্য ধন্যবাদ
  '0008': 'assets/sounds/0008.mp3', // আপনার সঙ্গে কথা বলার জন্য চিকিৎসক প্রস্তুত রয়েছেন
  '0009': 'assets/sounds/0009.mp3', // আগামী এক মিনিটের মধ্যে চিকিৎসকের সঙ্গে ভিডিও পরামর্শ শুরু হবে… ধন্যবাদ, সুস্থতা কামনা করছি
  '0010': 'assets/sounds/0010.mp3', // অনুগ্রহ করে আপনার জিনিসপত্র নিয়ে নিন — ONLY by the "Take your items" button on the Drive page
};
const voice = (() => {
  const queue = [];
  let current = null;
  function next() {
    const item = queue.shift();
    if (!item) { current = null; return; }
    const a = new Audio(SOUNDS[item.key]);
    a.dataset.sound = item.key;
    current = { key: item.key, audio: a };
    let finished = false;
    const done = () => {
      if (finished) return;
      finished = true;
      current = null;
      try { item.onEnd?.(); } catch { /* ignore */ }
      setTimeout(next, 300);
    };
    a.addEventListener('ended', done);
    a.addEventListener('error', done);
    const go = () => a.play().catch((err) => { console.warn('[voice]', item.key, err?.message || err); done(); });
    if (prefs.speakerId && typeof a.setSinkId === 'function') a.setSinkId(prefs.speakerId).then(go, go);
    else go();
  }
  return {
    // returns true if the sound will play
    play(key, opts = {}) {
      if (!SOUNDS[key]) return false;
      if (prefs.voice === 'off' && !opts.force) return false;
      if (consultQuiet() && !opts.force) return false;   // never talk over the doctor (see consultQuiet)
      if (current?.key === key || queue.some((q) => q.key === key)) return false;
      queue.push({ key, onEnd: opts.onEnd });
      if (!current) next();
      return true;
    },
    busy: () => !!current || queue.length > 0,
    // silence now: drop the queue and stop what is playing
    stopAll() {
      queue.length = 0;
      if (current) { try { current.audio.pause(); } catch { /* ignore */ } current = null; }
    },
  };
})();

// No automatic announcements during a video consultation, nor for 60 s after it
// (the patient is still sitting in front of the camera). "Take your items" (pressed on purpose) still plays.
const CONSULT_QUIET_AFTER_MS = 60000;
let quietUntil = 0;
function consultQuiet() { return !!S.call || Date.now() < quietUntil; }
function loadPrefs() {
  const base = { cameraId: '', micId: '', speakerId: '', autoAnswer: DEFAULT_AUTO_ANSWER_S, voice: 'on' };
  try { return { ...base, ...JSON.parse(localStorage.getItem(PREFS_KEY) || '{}') }; } catch { return base; }
}
function savePrefs() {
  try { localStorage.setItem(PREFS_KEY, JSON.stringify(prefs)); } catch { /* storage unavailable */ }
}

// ─── Auth gate ──────────────────────────────────────────────────
onAuthStateChanged(auth, (user) => {
  el('boot').hidden = true;
  S.user = user;
  if (!user) {
    if (S.started) { location.reload(); return; }
    el('kiosk').hidden = true;
    el('setup').hidden = false;
    return;
  }
  el('setup').hidden = true;
  el('kiosk').hidden = false;
  if (!S.started) start();
});

el('setupForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const msg = el('setupMsg');
  msg.hidden = true;
  const btn = el('setupBtn');
  btn.disabled = true; btn.textContent = 'Signing in…';
  try {
    await signInWithEmailAndPassword(auth, el('setupEmail').value.trim(), el('setupPass').value);
  } catch (err) {
    msg.textContent = authErrorText(err); msg.hidden = false;
  } finally {
    btn.disabled = false; btn.textContent = 'Sign in & start';
  }
});

// ─── Start ──────────────────────────────────────────────────────
async function start() {
  S.started = true;
  el('setCartId').textContent = CART_LABEL;
  el('setAccount').textContent = S.user.email || S.user.uid;
  el('setVersion').textContent = APP_VERSION;

  tickClock(); setInterval(tickClock, 10_000);
  initSettings();
  initCallUi();
  keepAwake();

  await acquireMedia();
  startPresence();
  startSignalling();
  startDriveBanner();
  startAnnouncements();
  startSensor();
  setInterval(renderChips, 2000);
  navigator.mediaDevices?.addEventListener?.('devicechange', () => { if (!S.call) acquireMedia({ quiet: true }); });
  // keep trying quietly while no camera is available (e.g. splitter still starting)
  setInterval(() => { if (!S.camLabel && !S.call) acquireMedia({ quiet: true }); }, 15_000);
}

function tickClock() {
  el('clock').textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

async function keepAwake() {
  const req = async () => {
    try { S.wakeLock = await navigator.wakeLock?.request('screen'); } catch { /* not granted — kiosk flags handle blanking */ }
  };
  await req();
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') req(); });
}

// ─── Camera & microphone ────────────────────────────────────────
async function hintedCameraId() {
  if (!CART_CAMERA_HINT) return '';
  try {
    const devs = await navigator.mediaDevices.enumerateDevices();
    return devs.find((d) => d.kind === 'videoinput' && d.label.includes(CART_CAMERA_HINT))?.deviceId || '';
  } catch { return ''; }
}

let acquiring = null;
function acquireMedia(opts = {}) {
  // serialise: devicechange, track-ended and settings can all ask at once
  acquiring = (acquiring || Promise.resolve()).then(() => doAcquire(opts)).catch((e) => console.warn('media', e));
  return acquiring;
}

async function doAcquire({ quiet = false, retried = false } = {}) {
  const old = S.local;
  const audio = { echoCancellation: true, noiseSuppression: true, autoGainControl: true };
  const camId = prefs.cameraId || await hintedCameraId();
  const tries = [
    { video: { ...CART_VIDEO, ...(camId ? { deviceId: { exact: camId } } : {}) }, audio: { ...audio, ...(prefs.micId ? { deviceId: { exact: prefs.micId } } : {}) } },
    { video: CART_VIDEO, audio },
    { audio },
  ];
  if (old) old.getTracks().forEach((t) => t.stop());   // free the device before reopening it
  let stream = null, lastErr = null;
  for (const c of tries) {
    try { stream = await navigator.mediaDevices.getUserMedia(c); break; } catch (err) { lastErr = err; }
  }
  S.local = stream;
  S.mediaError = stream ? null : (lastErr?.name || 'NoDevice');
  S.camLabel = stream?.getVideoTracks()[0]?.label || null;
  S.micLabel = stream?.getAudioTracks()[0]?.label || null;

  // first run: device labels only appear after permission — retry once to land on the call camera
  if (!retried && !prefs.cameraId && CART_CAMERA_HINT && S.camLabel && !S.camLabel.includes(CART_CAMERA_HINT) && await hintedCameraId()) {
    return doAcquire({ quiet, retried: true });
  }

  el('selfPreview').srcObject = stream && stream.getVideoTracks().length ? stream : null;
  el('kSelfWrap').hidden = !(stream && stream.getVideoTracks().length);
  el('callSelf').srcObject = stream;

  // the camera splitter may restart (USB hiccup) — pick the feed back up by itself
  stream?.getVideoTracks()[0]?.addEventListener('ended', () => {
    if (S.local !== stream) return;
    S.camLabel = null; renderChips();
    setTimeout(() => acquireMedia({ quiet: true }), 2000);
  });

  // hot-swap into a running call
  if (S.call?.pc && stream) {
    for (const sender of S.call.pc.getSenders()) {
      const kind = sender.track?.kind;
      const next = kind === 'video' ? stream.getVideoTracks()[0] : kind === 'audio' ? stream.getAudioTracks()[0] : null;
      if (next) sender.replaceTrack(next).catch(() => {});
    }
  }
  renderChips();
  pushPresence();
  if (quiet) return;
  if (!stream) toast('No camera or microphone available. Check the USB webcam.', 'crit', 8000);
  else if (!S.camLabel) toast('Camera unavailable — calls will be audio only.', 'warn', 6000);
}

// ─── Presence ───────────────────────────────────────────────────
function presencePayload() {
  return {
    online: true,
    lastSeen: serverTimestamp(),
    inCall: !!S.call,
    callDoctor: S.call?.doctorName || null,
    camera: S.camLabel,
    mic: S.micLabel,
    cameraOk: !!S.camLabel,
    sensorOnline: sensorOnline(),
    version: APP_VERSION,
    ua: navigator.userAgent.slice(0, 160),
  };
}
function pushPresence() {
  if (!S.connected) return;
  update(ref(rtdb, presencePath), presencePayload()).catch((e) => console.warn('presence', e));
}
function startPresence() {
  const presRef = ref(rtdb, presencePath);
  onValue(ref(rtdb, '.info/connected'), async (snap) => {
    S.connected = !!snap.val();
    renderChips();
    if (!S.connected) return;
    try {
      await onDisconnect(presRef).update({ online: false, inCall: false, lastSeen: serverTimestamp() });
      await update(presRef, presencePayload());
    } catch (e) { console.warn('presence', e); }
  });
  setInterval(() => {
    if (S.connected) update(presRef, { online: true, lastSeen: serverTimestamp(), sensorOnline: sensorOnline() }).catch(() => {});
  }, 30_000);
}

// ─── Sensor via MQTT + Firebase logging ─────────────────────────
const sensorOnline = () => Date.now() - S.lastSensorAt < SENSOR_STALE_MS;

function startSensor() {
  connectMqtt({
    onSensor: onSensor,
    onError: (e) => console.warn('[mqtt]', e?.message || e),
  });
  setInterval(() => { if (!sensorOnline()) renderVitals(); }, 2000);
}

let lastRendered = { hr: null, sp: null };
function onSensor(d) {
  const wasOnline = sensorOnline();
  S.vit = d;
  S.lastSensorAt = Date.now();
  renderVitals();
  vitalsVoice(d);
  if (!wasOnline) pushPresence();

  set(ref(rtdb, 'medicart/live'), {
    heartRate: d.heartRate, spo2: d.spo2, ir: d.ir, red: d.red,
    fingerDetected: d.fingerDetected, state: d.state, samplesCollected: d.samplesCollected,
    deviceId: d.deviceId, cartId: CART_ID, updatedAt: serverTimestamp(),
  }).catch((e) => console.warn('rtdb live', e));

  if (d.fingerDetected && Date.now() - S.lastHistoryAt >= HISTORY_INTERVAL_MS) {
    S.lastHistoryAt = Date.now();
    addDoc(collection(db, 'sensor_history'), {
      heartRate: d.heartRate, spo2: d.spo2, ir: d.ir, red: d.red,
      fingerDetected: d.fingerDetected, state: d.state, deviceId: d.deviceId, cartId: CART_ID,
      timestamp: fsTimestamp(),
    }).then(() => { S.logged += 1; el('setLogged').textContent = S.logged; })
      .catch((e) => console.warn('firestore history', e));
  }
}

// Pulse-oximeter announcements: finger placed → keep it still (0005), reading done → thank you (0006),
// then the consultation will start soon (0009, only when no call is on). Movement → keep still again.
const VS = { finger: false, measuringSaid: false, lockedSaid: false, lastStill: 0 };
function vitalsVoice(d) {
  if (!d.fingerDetected) { VS.finger = false; VS.measuringSaid = false; VS.lockedSaid = false; return; }
  VS.finger = true;
  const now = Date.now();
  if (d.state === 'measuring' && !VS.measuringSaid) { VS.measuringSaid = true; VS.lastStill = now; voice.play('0005'); }
  else if (d.motion && !VS.lockedSaid && now - VS.lastStill > 12000) { VS.lastStill = now; voice.play('0005'); }
  if (d.state === 'locked' && !VS.lockedSaid) {
    VS.lockedSaid = true;
    voice.play('0006');
    if (!S.call && !S.ring) voice.play('0009');
  }
}

function setNum(id, value, key) {
  const node = el(id);
  const text = value == null ? '--' : String(value);
  if (node.textContent !== text) {
    node.textContent = text;
    if (value != null && lastRendered[key] !== value) { node.classList.remove('pop'); void node.offsetWidth; node.classList.add('pop'); }
    lastRendered[key] = value;
  }
}

function renderVitals() {
  const on = sensorOnline();
  const d = on ? S.vit : null;
  const hr = d?.fingerDetected && d.heartRate > 0 ? d.heartRate : null;
  const sp = d?.fingerDetected && d.spo2 > 0 ? d.spo2 : null;
  setNum('kHR', hr, 'hr'); setNum('kSp', sp, 'sp');
  el('cHR').textContent = hr ?? '--';
  el('cSp').textContent = sp ?? '--';

  let tone = 'idle', en, bn, step;
  const n = d?.samplesCollected || 0;
  if (!on) {
    tone = 'error'; en = 'The vital sensor is offline'; bn = 'ভাইটাল সেন্সর অফলাইন'; step = 'Sensor offline';
  } else if (d.state === 'sensor_error') {
    tone = 'error'; en = 'Sensor needs attention — please call staff'; bn = 'সেন্সরে সমস্যা — অনুগ্রহ করে কর্মীকে ডাকুন'; step = 'Sensor error';
  } else if (!d.fingerDetected) {
    en = 'Place your fingertip on the sensor'; bn = 'সেন্সরে আপনার আঙুলের ডগা রাখুন'; step = 'Waiting for your finger';
  } else if (d.state === 'locked') {
    tone = 'done'; en = 'Reading complete'; bn = 'মাপা সম্পন্ন হয়েছে'; step = 'Done — you can remove your finger';
  } else {
    tone = 'measuring'; en = 'Hold still — measuring'; bn = 'স্থির থাকুন — মাপা হচ্ছে'; step = `Measuring… ${n} of 4`;
  }
  const box = el('kInstr');
  box.dataset.tone = tone;
  el('kInstrEn').textContent = en;
  el('kInstrBn').textContent = bn;
  el('kStepText').textContent = step;
  el('kSteps').querySelectorAll('i').forEach((i, k) => i.classList.toggle('on', on && k < n));
}

function renderChips() {
  const chip = (id, ok, label) => {
    const c = el(id);
    c.querySelector('.dot').className = `dot ${ok ? 'ok' : 'crit'}`;
    c.lastChild.textContent = label;
  };
  chip('chipNet', S.connected, S.connected ? 'Online' : 'Offline');
  chip('chipSensor', sensorOnline(), sensorOnline() ? 'Sensor' : 'No sensor');
  chip('chipCam', !!S.camLabel, S.camLabel ? 'Camera' : 'No camera');
  const wait = el('kWait');
  wait.classList.toggle('off', !S.connected);
  el('kWaitText').textContent = S.connected ? 'Waiting for a doctor to call' : 'No internet — reconnecting…';
}

// ════════════════════════════════════════════════════════════════
//  Signalling — the cart is the callee
// ════════════════════════════════════════════════════════════════
function startSignalling() {
  onValue(ref(rtdb, callPath), (snap) => onCallNode(snap.val()));
}

// Drive mode banner: tells people near the cart that someone is driving it by hand,
// or that the emergency stop is on (state written by medicart-control on the Pi).
function startDriveBanner() {
  let st = null;
  const show = () => {
    const fresh = st && st.updatedAt && serverNow() - st.updatedAt < 6000;
    const box = el('kDrive');
    if (!fresh || st.mode === 'AUTO') { box.hidden = true; return; }
    const stop = st.mode === 'STOP';
    box.classList.toggle('stop', stop);
    el('kDriveEn').textContent = stop ? 'Emergency stop' : 'Manual driving';
    el('kDriveBn').textContent = stop ? 'জরুরি থামানো' : 'ম্যানুয়াল চালনা';
    el('kDriveBy').textContent = stop ? '' : (st.driver ? `· ${st.driver}` : '');
    box.hidden = false;
  };
  onValue(ref(rtdb, `carts/${CART_ID}/control/state`), (snap) => { st = snap.val(); show(); blockedVoice(st); arrivalVoice(st); });
  setInterval(() => { show(); blockedVoice(st); arrivalVoice(st); }, 1000);
}

// A person in the cart's stop zone while it is trying to drive: Auto (line following), or Manual while
// an operator is pressing a drive key. "Please step aside" (0001); still there after 8 s:
// "please keep the area clear" (0002), then every 20 s. Silent during a consultation.
const BV = { since: 0, last: 0, count: 0, last0001: 0 };
function blockedVoice(st) {
  const fresh = st && st.updatedAt && serverNow() - st.updatedAt < 6000;
  const blocked = fresh && !consultQuiet() &&
    ((st.mode === 'AUTO' && st.vision === 'blocked') || (st.mode === 'MANUAL' && st.blocked === true));
  const now = Date.now();
  if (!blocked) { BV.since = 0; return; }
  if (!BV.since) {
    BV.since = now; BV.last = now; BV.count = 0;
    if (now - BV.last0001 > 15000) { BV.last0001 = now; voice.play('0001'); }
    return;
  }
  if (now - BV.last >= (BV.count === 0 ? 8000 : 20000)) { BV.last = now; BV.count += 1; voice.play('0002'); }
}

// Arrival: the cart drove, then stood still for 3 s (not because a person is in the way):
// "place your index finger on the sensor" (0004). Once per stop, when the pulse sensor is online,
// no finger is on it yet and no call is on.
const AV = { moved: false, stillSince: 0 };
function arrivalVoice(st) {
  const fresh = st && st.updatedAt && serverNow() - st.updatedAt < 6000;
  if (!fresh) return;
  const a = st.applied || {};
  const now = Date.now();
  if (Math.abs(a.l || 0) + Math.abs(a.r || 0) > 0) { AV.moved = true; AV.stillSince = 0; return; }
  if (!AV.moved) return;
  if (st.vision === 'blocked') { AV.stillSince = 0; return; }      // stopped for a person, not arrived
  if (!AV.stillSince) { AV.stillSince = now; return; }
  if (now - AV.stillSince < 3000) return;
  if (!sensorOnline() || S.vit?.fingerDetected || S.call || S.ring) return;
  AV.moved = false;
  voice.play('0004');
}

// "Take your items" on the console's Drive page (the only announcement started by a person): carts/<id>/announce {id, sound, at, name}
function startAnnouncements() {
  const seen = new Set();
  const path = `carts/${CART_ID}/announce`;
  onValue(ref(rtdb, path), (snap) => {
    const a = snap.val();
    if (!a || !a.id || !a.sound || seen.has(a.id) || a.playedId === a.id) return;
    seen.add(a.id);
    if (a.at && serverNow() - a.at > 15000) return;          // an old request (screen restarted)
    if (a.sound === '0010') toast('অনুগ্রহ করে আপনার জিনিসপত্র নিন · Please take your items', 'ok', 8000);
    voice.play(a.sound, {
      force: true,
      onEnd: () => update(ref(rtdb, path), { playedId: a.id, playedAt: serverTimestamp() }).catch(() => {}),
    });
  });
}

function onCallNode(d) {
  if (!d) {
    if (S.ring) stopRinging();
    if (S.call) endCall('cancelled', false);
    return;
  }
  // a different call replaced ours
  if (S.call && d.callId !== S.call.id) endCall('replaced', false);
  if (S.ring && d.callId !== S.ring.callId) stopRinging();

  if (d.status === 'ringing' && !S.call && !S.ring && !S.handled.has(d.callId)) {
    const age = d.createdAt ? serverNow() - d.createdAt : 0;
    if (age < 60_000) startRinging(d);
    else S.handled.add(d.callId);
  }
  if (d.status === 'ended' || d.status === 'declined') {
    if (S.ring?.callId === d.callId) { stopRinging(); toast('The doctor cancelled the call.'); }
    if (S.call?.id === d.callId && d.endedBy !== 'cart') endCall('doctor', false);
  }
  if (S.call?.id === d.callId && d.prompt && d.prompt.id !== S.lastPromptId) {
    S.lastPromptId = d.prompt.id;
    showPrompt(d.prompt);
  }
}

// ─── Ringing ────────────────────────────────────────────────────
function startRinging(d) {
  const name = d.doctor?.name || 'Doctor';
  S.ring = { callId: d.callId, data: d, timer: null, countdown: null, audio: null };
  const ringId = d.callId;
  const startTone = () => { if (S.ring?.callId === ringId && !S.ring.audio) S.ring.audio = ringtone(); };
  if (!voice.play('0008', { onEnd: startTone })) startTone();   // "the doctor is ready to talk with you"
  S.handled.add(d.callId);
  el('incName').textContent = name;
  el('incoming').hidden = false;
  const auto = Number(prefs.autoAnswer) || 0;
  if (auto > 0) {
    let left = auto;
    const paint = () => { el('incAuto').textContent = `Connecting automatically in ${left}… · স্বয়ংক্রিয়ভাবে সংযোগ হচ্ছে`; };
    paint();
    S.ring.countdown = setInterval(() => {
      left -= 1;
      if (left <= 0) {
        clearInterval(S.ring.countdown);
        const until = Date.now() + 20000;           // never wait more than 20 s
        const go = () => { if (S.ring?.callId !== ringId) return; if (voice.busy() && Date.now() < until) setTimeout(go, 250); else answer(); };
        go();                                       // after the announcement has finished
      } else paint();
    }, 1000);
  } else {
    el('incAuto').textContent = 'Tap Answer to talk to the doctor · কথা বলতে "কল ধরুন" চাপুন';
  }
  pushPresence();
}

function stopRinging() {
  if (!S.ring) return;
  clearInterval(S.ring.countdown);
  S.ring.audio?.stop();
  S.ring = null;
  el('incoming').hidden = true;
}

function ringtone() {
  try {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    const ctx = new Ctx();
    const gain = ctx.createGain(); gain.gain.value = 0; gain.connect(ctx.destination);
    const o1 = ctx.createOscillator(), o2 = ctx.createOscillator();
    o1.frequency.value = 523.25; o2.frequency.value = 659.25; // C5 + E5
    o1.connect(gain); o2.connect(gain); o1.start(); o2.start();
    let on = false;
    const iv = setInterval(() => {
      on = !on;
      const t = ctx.currentTime;
      gain.gain.cancelScheduledValues(t);
      gain.gain.setTargetAtTime(on ? 0.12 : 0, t, 0.03);
    }, 700);
    return { stop() { clearInterval(iv); try { o1.stop(); o2.stop(); ctx.close(); } catch { /* closed */ } } };
  } catch { return null; }
}

function initCallUi() {
  el('answerBtn').addEventListener('click', answer);
  el('declineBtn').addEventListener('click', async () => {
    const r = S.ring; if (!r) return;
    stopRinging();
    await update(ref(rtdb, callPath), { status: 'declined', endedBy: 'cart', endedAt: serverTimestamp() }).catch(() => {});
    pushPresence();
  });
  el('endBtn').addEventListener('click', () => endCall('patient', true));
  el('tapSound').addEventListener('click', () => {
    el('remoteVideo').play().then(() => { el('tapSound').hidden = true; }).catch(() => {});
  });
}

// ─── Answer ─────────────────────────────────────────────────────
async function answer() {
  const r = S.ring; if (!r) return;
  stopRinging();
  voice.stopAll();                                  // the consultation starts: silence
  if (!S.local) await acquireMedia();

  const d = r.data;
  const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
  const C = S.call = {
    id: d.callId, pc, doctorName: d.doctor?.name || 'Doctor', connectedAt: null,
    unsubs: [], timer: null, dropTimer: null, disc: null, ended: false,
  };
  el('callDocName').textContent = C.doctorName;
  el('callTimer').textContent = '00:00';
  el('callConn').hidden = false;
  el('callConnText').textContent = 'Connecting to the doctor… · ডাক্তারের সাথে সংযোগ হচ্ছে';
  el('call').hidden = false;

  if (S.local) S.local.getTracks().forEach((t) => pc.addTrack(t, S.local));
  if (!S.local?.getVideoTracks().length) pc.addTransceiver('video', { direction: 'recvonly' });
  if (!S.local?.getAudioTracks().length) pc.addTransceiver('audio', { direction: 'recvonly' });

  pc.ontrack = (e) => {
    const v = el('remoteVideo');
    if (v.srcObject !== e.streams[0]) {
      v.srcObject = e.streams[0] || new MediaStream([e.track]);
      if (prefs.speakerId && typeof v.setSinkId === 'function') v.setSinkId(prefs.speakerId).catch(() => {});
      v.play().catch(() => { el('tapSound').hidden = false; });
    }
  };
  pc.onicecandidate = (e) => {
    if (e.candidate) push(ref(rtdb, `${callPath}/answerCandidates`), packCandidate(e.candidate, C.id)).catch(() => {});
  };
  pc.onconnectionstatechange = () => {
    const st = pc.connectionState;
    if (st === 'connected') {
      clearTimeout(C.dropTimer);
      el('callConn').hidden = true;
      if (!C.connectedAt) {
        C.connectedAt = Date.now();
        C.timer = setInterval(() => { el('callTimer').textContent = fmtClock((Date.now() - C.connectedAt) / 1000); }, 1000);
      }
    } else if (st === 'disconnected') {
      el('callConn').hidden = false;
      el('callConnText').textContent = 'Connection weak — reconnecting… · সংযোগ দুর্বল';
      C.dropTimer = setTimeout(() => { if (pc.connectionState !== 'connected') endCall('lost', true); }, 15_000);
    } else if (st === 'failed') {
      endCall('failed', true);
    }
  };

  try {
    await pc.setRemoteDescription(d.offer);
    const ans = await pc.createAnswer();
    await pc.setLocalDescription(ans);
    await update(ref(rtdb, callPath), { answer: { type: ans.type, sdp: ans.sdp }, status: 'active', answeredAt: serverTimestamp() });
    C.disc = onDisconnect(ref(rtdb, callPath));
    await C.disc.update({ status: 'ended', endedBy: 'cart', endReason: 'cart_offline' });
    C.unsubs.push(onChildAdded(ref(rtdb, `${callPath}/offerCandidates`), (snap) => {
      const v = snap.val();
      if (v && v.callId === C.id) pc.addIceCandidate(unpackCandidate(v)).catch((e) => console.warn('ice', e));
    }));
    pushPresence();
  } catch (err) {
    console.error(err);
    toast('Could not connect the call.', 'crit');
    endCall('failed', true);
  }
}

// ─── End ────────────────────────────────────────────────────────
async function endCall(reason, notifyDoctor) {
  const C = S.call;
  if (!C || C.ended) return;
  C.ended = true;
  S.call = null;
  quietUntil = Date.now() + CONSULT_QUIET_AFTER_MS;
  BV.since = 0; AV.moved = false; AV.stillSince = 0; VS.measuringSaid = VS.lockedSaid = true;   // no catch-up after the call
  if (notifyDoctor) {
    await update(ref(rtdb, callPath), { status: 'ended', endedBy: 'cart', endReason: reason, endedAt: serverTimestamp() }).catch(() => {});
  }
  clearInterval(C.timer); clearTimeout(C.dropTimer);
  C.unsubs.forEach((u) => u());
  C.disc?.cancel().catch(() => {});
  try { C.pc.close(); } catch { /* closed */ }
  el('remoteVideo').srcObject = null;
  el('call').hidden = true;
  el('callPrompt').hidden = true;
  el('tapSound').hidden = true;
  pushPresence();
  const msg = {
    doctor: 'The doctor ended the call. Thank you! · ধন্যবাদ!',
    patient: 'Call ended. Thank you! · ধন্যবাদ!',
    failed: 'The call could not connect. The doctor can try again.',
    lost: 'The connection was lost.',
  }[reason];
  if (msg) toast(msg, reason === 'failed' || reason === 'lost' ? 'warn' : 'ok', 5000);
}

// ─── Prompts from the doctor ────────────────────────────────────
let promptTimer = null;
function showPrompt(p) {
  el('promptEn').textContent = p.en || '';
  el('promptBn').textContent = p.bn || '';
  el('promptBn').hidden = !p.bn;
  const box = el('callPrompt');
  box.hidden = false;
  box.style.animation = 'none'; void box.offsetWidth; box.style.animation = '';
  clearTimeout(promptTimer);
  promptTimer = setTimeout(() => { box.hidden = true; }, 9000);
  chime();
}
function chime() {
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    const o = ctx.createOscillator(), g = ctx.createGain();
    o.frequency.value = 880; g.gain.setValueAtTime(0.0001, ctx.currentTime);
    g.gain.exponentialRampToValueAtTime(0.15, ctx.currentTime + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + 0.6);
    o.connect(g); g.connect(ctx.destination); o.start(); o.stop(ctx.currentTime + 0.65);
    o.onended = () => ctx.close();
  } catch { /* audio unavailable */ }
}

// ─── Settings (press & hold the gear for 1.5 s) ─────────────────
function initSettings() {
  const gear = el('gearBtn');
  let t0 = 0, raf = 0;
  const reset = () => { cancelAnimationFrame(raf); t0 = 0; el('holdRing').style.setProperty('--p', 0); };
  const step = () => {
    const p = Math.min(100, ((performance.now() - t0) / 1500) * 100);
    el('holdRing').style.setProperty('--p', p);
    if (p >= 100) { reset(); openSettings(); return; }
    raf = requestAnimationFrame(step);
  };
  gear.addEventListener('pointerdown', (e) => { e.preventDefault(); t0 = performance.now(); raf = requestAnimationFrame(step); });
  ['pointerup', 'pointerleave', 'pointercancel'].forEach((ev) => gear.addEventListener(ev, reset));
  gear.addEventListener('contextmenu', (e) => e.preventDefault());

  el('setClose').addEventListener('click', () => { el('settings').hidden = true; });
  el('settings').addEventListener('click', (e) => { if (e.target === el('settings')) el('settings').hidden = true; });
  el('setCam').addEventListener('change', async (e) => { prefs.cameraId = e.target.value; savePrefs(); await acquireMedia(); fillDevices(); });
  el('setMic').addEventListener('change', async (e) => { prefs.micId = e.target.value; savePrefs(); await acquireMedia(); fillDevices(); });
  el('setSpk').addEventListener('change', (e) => {
    prefs.speakerId = e.target.value; savePrefs();
    const v = el('remoteVideo');
    if (typeof v.setSinkId === 'function') v.setSinkId(prefs.speakerId).catch(() => {});
  });
  el('setAuto').addEventListener('change', (e) => { prefs.autoAnswer = Number(e.target.value); savePrefs(); });
  el('setVoice').addEventListener('change', (e) => { prefs.voice = e.target.value; savePrefs(); });
  el('setVoiceTest').addEventListener('click', () => { if (!voice.play('0008', { force: true })) toast('A voice announcement is already playing.'); });
  el('setTest').addEventListener('click', testSpeaker);
  el('setReload').addEventListener('click', () => location.reload());
  el('setSignOut').addEventListener('click', async () => {
    await update(ref(rtdb, presencePath), { online: false, inCall: false, lastSeen: serverTimestamp() }).catch(() => {});
    await signOut(auth);
  });
}

async function openSettings() {
  el('setAuto').value = String(prefs.autoAnswer);
  el('setVoice').value = prefs.voice === 'off' ? 'off' : 'on';
  if (![...el('setAuto').options].some((o) => o.value === String(prefs.autoAnswer))) el('setAuto').value = '3';
  el('settings').hidden = false;
  await fillDevices();
}

async function fillDevices() {
  let devices = [];
  try { devices = await navigator.mediaDevices.enumerateDevices(); } catch { /* none */ }
  const fill = (id, kind, current, fallbackLabel) => {
    const list = devices.filter((d) => d.kind === kind);
    const sel = el(id);
    sel.innerHTML = '';
    const def = document.createElement('option');
    def.value = ''; def.textContent = 'System default';
    sel.appendChild(def);
    list.forEach((d, i) => {
      const o = document.createElement('option');
      o.value = d.deviceId; o.textContent = d.label || `${fallbackLabel} ${i + 1}`;
      sel.appendChild(o);
    });
    sel.value = list.some((d) => d.deviceId === current) ? current : '';
  };
  fill('setCam', 'videoinput', prefs.cameraId, 'Camera');
  fill('setMic', 'audioinput', prefs.micId, 'Microphone');
  const canSink = typeof HTMLMediaElement !== 'undefined' && 'setSinkId' in HTMLMediaElement.prototype;
  el('setSpkWrap').hidden = !canSink;
  if (canSink) fill('setSpk', 'audiooutput', prefs.speakerId, 'Speaker');
}

async function testSpeaker() {
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    if (prefs.speakerId && typeof ctx.setSinkId === 'function') await ctx.setSinkId(prefs.speakerId).catch(() => {});
    const o = ctx.createOscillator(), g = ctx.createGain();
    o.frequency.value = 660; g.gain.value = 0.15;
    o.connect(g); g.connect(ctx.destination); o.start();
    setTimeout(() => { o.stop(); ctx.close(); }, 700);
  } catch { toast('Audio output unavailable.', 'warn'); }
}
}

if (window.MC_FB) main();
else window.addEventListener('mc-ready', main, { once: true });
})();
