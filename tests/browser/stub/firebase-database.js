// Test stub: Realtime Database over the fake Firebase REST/SSE server (window.__TEST_FB__)
const BASE = () => window.__TEST_FB__;
const TOK = 'tok-browser';
const url = (p) => `${BASE()}/${p.replace(/^\/+/, '')}.json?auth=${TOK}`;
export function getDatabase(app) { return { app }; }
export function ref(db, path = '') { const p = String(path).replace(/^\/+|\/+$/g, ''); return { path: p, key: p.split('/').pop() || null }; }
export function serverTimestamp() { return { '.sv': 'timestamp' }; }
async function send(method, r, v) {
  const res = await fetch(url(r.path), { method, body: JSON.stringify(v === undefined ? null : v) });
  if (!res.ok) throw new Error('PERMISSION_DENIED');
}
export const set = (r, v) => send('PUT', r, v);
export const update = (r, v) => send('PATCH', r, v);
export const remove = (r) => send('PUT', r, null);
export function push(r, v) {
  const c = ref(null, `${r.path}/k${Date.now().toString(36)}${Math.random().toString(36).slice(2, 7)}`);
  const p = v === undefined ? Promise.resolve() : set(c, v);
  return Object.assign(p.then(() => c), c);
}
function snap(v, key) {
  return { val: () => v, exists: () => v !== null && v !== undefined, key,
    forEach(cb) { if (v && typeof v === 'object') Object.entries(v).forEach(([k, x]) => cb(snap(x, k))); } };
}
export async function get(r) { const res = await fetch(url(r.path)); return snap(await res.json(), r.key); }
function setPath(obj, path, val) {
  const parts = path.split('/').filter(Boolean);
  if (!parts.length) return val;
  const root = obj && typeof obj === 'object' ? { ...obj } : {};
  let n = root;
  for (let i = 0; i < parts.length - 1; i++) { n[parts[i]] = n[parts[i]] && typeof n[parts[i]] === 'object' ? { ...n[parts[i]] } : {}; n = n[parts[i]]; }
  if (val === null) delete n[parts[parts.length - 1]]; else n[parts[parts.length - 1]] = val;
  return root;
}
// One shared EventSource on the root for all listeners (like the real SDK's single connection):
// a browser allows only 6 connections per host, so one stream per listener blocked writes.
let tree = null, stream = null;
const listeners = new Set();
function at(path) {
  let n = tree;
  for (const part of path.split('/').filter(Boolean)) { if (!n || typeof n !== 'object') return null; n = n[part]; }
  return n === undefined ? null : n;
}
function notify() {
  for (const l of listeners) {
    const v = at(l.path); const j = JSON.stringify(v);
    if (j !== l.last) { l.last = j; l.cb(snap(v, l.key)); }
  }
}
function ensureStream() {
  if (stream) return;
  stream = new EventSource(url(''));
  stream.addEventListener('put', (e) => { const m = JSON.parse(e.data); tree = setPath(tree, m.path, m.data); notify(); });
  stream.addEventListener('patch', (e) => { const m = JSON.parse(e.data); for (const [k, v] of Object.entries(m.data || {})) tree = setPath(tree, `${m.path === '/' ? '' : m.path}/${k}`, v); notify(); });
}
export function onValue(r, cb) {
  if (r.path.startsWith('.info')) { setTimeout(() => cb(snap(r.path.endsWith('connected') ? true : 0, null)), 0); return () => {}; }
  const l = { path: r.path, key: r.key, cb, last: undefined };
  listeners.add(l);
  if (stream && tree !== null) setTimeout(() => { const v = at(l.path); l.last = JSON.stringify(v); cb(snap(v, l.key)); }, 0);
  ensureStream();
  return () => listeners.delete(l);
}
export function onChildAdded(r, cb) {
  const seen = new Set();
  return onValue(r, (s) => { const v = s.val() || {}; for (const [k, x] of Object.entries(v)) if (!seen.has(k)) { seen.add(k); cb(snap(x, k)); } });
}
export function onDisconnect() { return { set: async () => {}, update: async () => {}, remove: async () => {}, cancel: async () => {} }; }
