// MediCart Pi dashboard — talks to web_dashboard.py (Rifat's API):
//   GET /api/status · POST /api/mode {mode, stop_token} · POST /api/stop · POST /api/speed · POST /api/control {forward, turn, keys}
// Manual driving: while a key / the knob is held, /api/control is sent every 100 ms (the robot's lease is 0.5 s).
(() => {
  const $ = (id) => document.getElementById(id);
  const S = { st: null, held: false, keys: new Set(), x: 0, y: 0, timer: null, lastOk: 0 };
  const KEYMAP = { KeyW: 'up', ArrowUp: 'up', KeyS: 'down', ArrowDown: 'down', KeyA: 'left', ArrowLeft: 'left', KeyD: 'right', ArrowRight: 'right' };

  async function api(path, body) {
    const res = await fetch(path, body === undefined ? { cache: 'no-store' } : {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    });
    let data = {};
    try { data = await res.json(); } catch { /* empty */ }
    if (!res.ok) { const e = new Error(data.error || `HTTP ${res.status}`); e.status = res.status; throw e; }
    return data;
  }

  // ── status ─────────────────────────────────────────────
  async function poll() {
    try {
      S.st = await api('/api/status');
      S.lastOk = Date.now();
    } catch { S.st = null; }
    render();
  }
  setInterval(poll, 250);
  poll();

  function pill(el, text, tone) { el.textContent = text; el.className = `pill ${tone || ''}`; }

  function render() {
    const st = S.st;
    const online = st && Date.now() - S.lastOk < 2000;
    if (!online) {
      pill($('pMode'), 'Robot not answering', 'crit');
      $('note').textContent = 'The robot program (medicart.py) is not answering. Check: sudo systemctl status medicart-vision';
      lock(true);
      return;
    }
    const mode = st.mode;
    pill($('pMode'), mode === 'stopped' ? 'STOPPED' : mode.toUpperCase(), mode === 'stopped' ? 'crit' : mode === 'manual' ? 'warn' : 'info');
    pill($('pSerial'), st.serial === 'connected' ? 'Arduino OK' : `Arduino ${st.serial}`, st.serial === 'connected' ? 'ok' : 'crit');
    const vOff = st.video_age === null || st.video_age > 3;
    const blocked = st.vision === 'STOP' || st.vision === 'PERSON_DETECTED';
    pill($('pVision'), vOff ? 'Camera off' : blocked ? 'Person: STOP' : `Vision ${st.vision}`, vOff ? 'crit' : blocked ? 'crit' : st.vision === 'WARNING' ? 'warn' : 'ok');
    $('videoOff').hidden = !vOff;
    $('bAuto').classList.toggle('on', mode === 'auto');
    $('bManual').classList.toggle('on', mode === 'manual');
    $('bStop').classList.toggle('latched', mode === 'stopped');
    $('note').textContent = mode === 'stopped' ? `Stopped (${st.mode_by}). Choose Auto or Manual to move.`
      : mode === 'auto' ? `Auto: the robot follows the line; the camera stops it for people. Set by ${st.mode_by}.`
      : `Manual: drive with the pad or W A S D. Set by ${st.mode_by}.`;

    $('vPeople').textContent = `${st.people} ${st.people === 1 ? 'person' : 'people'}`;
    $('vNear').textContent = st.nearest_distance == null ? 'nearest —' : `nearest ${st.nearest_distance.toFixed(2)} m`;
    $('vFps').textContent = `${st.fps} fps`;
    $('sVision').textContent = st.vision;
    $('sInf').textContent = st.inference_ms == null ? '—' : `${Math.round(st.inference_ms)} ms`;
    $('sLat').textContent = st.frame_latency_ms == null ? '—' : `${Math.round(st.frame_latency_ms)} ms`;
    $('sFw').textContent = st.firmware || '—';
    $('sKeys').textContent = st.keys;
    const line = st.line || '';
    $('sLine').querySelectorAll('i').forEach((i, n) => i.classList.toggle('on', line[n] === '1'));
    $('speedMax').textContent = st.max_speed;
    $('speed').max = st.max_speed;
    if (document.activeElement !== $('speed')) { $('speed').value = st.speed; $('speedVal').textContent = st.speed; }
    wheels(st.motor_left, st.motor_right);

    const alert = $('alert');
    let msg = '', crit = false;
    if (mode === 'manual' && st.blocked) { msg = 'Person in the stop zone: the robot will not move until the path is clear.'; crit = true; }
    else if (mode === 'manual' && vOff) { msg = 'Camera / vision not running: manual driving is blocked.'; crit = true; }
    else if (mode === 'manual' && st.driver && S.held === false && (st.motor_left || st.motor_right)) msg = `${st.driver} is driving.`;
    alert.hidden = !msg; alert.textContent = msg; alert.classList.toggle('crit', crit);
    lock(mode !== 'manual');

    const ev = (st.events || []).slice().reverse();
    $('events').innerHTML = ev.length ? ev.map((e) => `<li><time>${new Date(e.t).toLocaleTimeString()}</time><span>${esc(e.text)}</span></li>`).join('')
      : '<li class="empty">No events yet.</li>';
  }
  function esc(t) { return String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
  function wheels(l, r) {
    [['L', l], ['R', r]].forEach(([k, v]) => {
      const pct = Math.min(1, Math.abs(v || 0) / 180) * 50;
      const bar = $(`w${k}`);
      bar.style.width = `${pct}%`; bar.style.left = (v || 0) >= 0 ? '50%' : `${50 - pct}%`;
      bar.classList.toggle('rev', (v || 0) < 0);
      $(`w${k}v`).textContent = v || 0;
    });
  }
  function lock(locked) {
    $('pad').classList.toggle('locked', locked);
    if (locked && S.held) release();
  }

  // ── mode / stop / speed ────────────────────────────────
  async function setMode(mode) {
    release();
    try {
      const st = S.st || await api('/api/status');
      await api('/api/mode', { mode, stop_token: st.stop_token, name: 'Pi dashboard' });
    } catch (e) {
      $('note').textContent = e.status === 409 ? 'An emergency stop happened just now. Press the mode again to confirm.' : `Could not change mode: ${e.message}`;
    }
    poll();
  }
  $('bAuto').addEventListener('click', () => setMode('auto'));
  $('bManual').addEventListener('click', () => setMode('manual'));
  async function estop() { release(); try { await api('/api/stop', { name: 'Pi dashboard' }); } catch { /* shown by poll */ } poll(); }
  $('bStop').addEventListener('click', estop);
  let speedTimer = null;
  $('speed').addEventListener('input', () => {
    $('speedVal').textContent = $('speed').value;
    clearTimeout(speedTimer);
    speedTimer = setTimeout(() => api('/api/speed', { speed: Number($('speed').value) }).catch(() => {}), 150);
  });

  // ── driving ────────────────────────────────────────────
  function send() {
    const keys = [...S.keys];
    api('/api/control', { forward: S.y, turn: S.x, keys, name: 'Pi dashboard' }).catch(() => {});
  }
  function start() {
    if (S.timer) return;
    send();
    S.timer = setInterval(send, 100);
  }
  function release() {
    const was = S.held || S.timer;
    S.held = false; S.keys.clear(); S.x = 0; S.y = 0;
    if (S.timer) { clearInterval(S.timer); S.timer = null; }
    $('knob').style.transform = '';
    $('pad').classList.remove('held');
    if (was) send();
  }
  function canDrive() { return S.st && S.st.mode === 'manual'; }
  function knob(x, y) { $('knob').style.transform = `translate(${x * 135}%, ${-y * 135}%)`; }

  const pad = $('pad');
  const move = (e) => {
    const r = pad.getBoundingClientRect();
    const R = (r.width / 2) * 0.72;
    let x = (e.clientX - (r.left + r.width / 2)) / R;
    let y = -(e.clientY - (r.top + r.height / 2)) / R;
    const m = Math.hypot(x, y); if (m > 1) { x /= m; y /= m; }
    S.x = Math.round(x * 100) / 100; S.y = Math.round(y * 100) / 100;
    knob(S.x, S.y);
  };
  pad.addEventListener('pointerdown', (e) => {
    if (!canDrive()) { $('note').textContent = 'Choose Manual first.'; return; }
    pad.setPointerCapture(e.pointerId); S.held = true; pad.classList.add('held'); move(e); start();
  });
  pad.addEventListener('pointermove', (e) => { if (S.held && !S.keys.size) move(e); });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((t) => pad.addEventListener(t, () => { if (!S.keys.size) release(); }));

  function fromKeys() {
    const k = S.keys;
    S.y = (k.has('up') ? 1 : 0) - (k.has('down') ? 1 : 0);
    S.x = (k.has('right') ? 1 : 0) - (k.has('left') ? 1 : 0);
    if (S.y && S.x) S.x *= 0.6;            // gentle arc when going forward + turning
    knob(S.x, S.y);
  }
  window.addEventListener('keydown', (e) => {
    if (e.target.closest && e.target.closest('input')) return;
    if (e.code === 'Escape') { e.preventDefault(); estop(); return; }
    if (e.code === 'Space') { e.preventDefault(); release(); return; }
    const k = KEYMAP[e.code]; if (!k) return;
    e.preventDefault();
    if (!canDrive()) { $('note').textContent = 'Choose Manual first.'; return; }
    S.keys.add(k); S.held = true; pad.classList.add('held'); fromKeys(); start();
  });
  window.addEventListener('keyup', (e) => {
    const k = KEYMAP[e.code]; if (!k) return;
    S.keys.delete(k);
    if (S.keys.size) fromKeys(); else release();
  });
  window.addEventListener('blur', release);
  document.addEventListener('visibilitychange', () => { if (document.hidden) release(); });
  $('video').addEventListener('error', () => { $('videoOff').hidden = false; setTimeout(() => { $('video').src = `/video_feed?${Date.now()}`; }, 2000); });
})();
