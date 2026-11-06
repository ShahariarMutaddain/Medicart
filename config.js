// ════════════════════════════════════════════════════════════════
//  MediCart — shared configuration (doctor console + cart screen)
//  Plain script (not a module) so the pages also open from disk.
//  Edit this file, then redeploy. Nothing here is a secret except
//  the MQTT topic prefix — see README "Security checklist".
// ════════════════════════════════════════════════════════════════

(function () {
const APP_VERSION = '2.0.0';

// Which cart this deployment talks to. One console can serve one cart;
// for a second cart, deploy a copy with CART_ID = 'medicart-02'.
const CART_ID = 'medicart-01';
const CART_LABEL = 'MediCart-01';

// ─── Firebase (project medicart-e20e0) ──────────────────────────
const FIREBASE_CONFIG = {
  apiKey: 'AIzaSyAjDepmO5m_fiWIy_HPcVnN2uHtMwGPv7M',
  authDomain: 'medicart-e20e0.firebaseapp.com',
  databaseURL: 'https://medicart-e20e0-default-rtdb.asia-southeast1.firebasedatabase.app',
  projectId: 'medicart-e20e0',
  storageBucket: 'medicart-e20e0.firebasestorage.app',
  messagingSenderId: '564687806819',
  appId: '1:564687806819:web:66f668912da11b0a6e7356',
};

// ─── MQTT (HiveMQ public broker, browser secure WebSocket) ─────
// broker.hivemq.com is free, needs no account and no login — and is
// shared with everyone. The long random topic prefix is what keeps
// strangers off MediCart's topics: keep it secret, and it MUST match
// TOPIC_PREFIX in esp32_sensor/medicart_sensor/medicart_sensor.ino.
// (For a private broker, put its wss:// URL and a login here.)
const MQTT_TOPIC_PREFIX = 'iub-medicart-591a7e01d2';
const MQTT = {
  url: 'wss://broker.hivemq.com:8884/mqtt',
  username: '',
  password: '',
  clientPrefix: 'medicart-web-',
  topics: {
    sensors: `${MQTT_TOPIC_PREFIX}/01/sensors`,
    status: `${MQTT_TOPIC_PREFIX}/01/status`,
  },
};

// ─── WebRTC ─────────────────────────────────────────────────────
// STUN finds a direct path; TURN (Metered, app "medicart-iub") relays
// the call when networks block direct video (campus Wi-Fi ↔ mobile data).
// Free plan: 500 MB/month of RELAYED traffic — direct calls use none.
// These TURN credentials are visible in the browser; if they leak, delete
// the credential in Metered → TURN Server and paste a new one here.
const TURN_USER = 'c8b125954b0e9dd71c8e6b68';
const TURN_PASS = 'GjC7Op6UbW6YS7JE';
const ICE_SERVERS = [
  { urls: ['stun:stun.l.google.com:19302', 'stun:stun.cloudflare.com:3478'] },
  { urls: 'stun:stun.relay.metered.ca:80' },
  { urls: 'turn:global.relay.metered.ca:80', username: TURN_USER, credential: TURN_PASS },
  { urls: 'turn:global.relay.metered.ca:80?transport=tcp', username: TURN_USER, credential: TURN_PASS },
  { urls: 'turn:global.relay.metered.ca:443', username: TURN_USER, credential: TURN_PASS },
  { urls: 'turns:global.relay.metered.ca:443?transport=tcp', username: TURN_USER, credential: TURN_PASS },
];

// Cart camera — kept modest so the Pi still has CPU for YOLO.
const CART_VIDEO = {
  width: { ideal: 640 },
  height: { ideal: 480 },
  frameRate: { ideal: 24, max: 30 },
};

// On the Pi, one USB camera is split into two virtual cameras by the
// medicart-camera service: "MediCart-Vision" (OpenCV/YOLO) and
// "MediCart-Call" (this page). The cart screen picks the one whose name
// contains this text unless a camera was chosen in its settings.
const CART_CAMERA_HINT = 'MediCart-Call';

// Doctor camera.
const DOCTOR_VIDEO = {
  width: { ideal: 1280 },
  height: { ideal: 720 },
  frameRate: { ideal: 30 },
};

const RING_TIMEOUT_MS = 45_000;       // doctor gives up after this
const DEFAULT_AUTO_ANSWER_S = 3;      // cart auto-answers (0 = off)
const PRESENCE_TIMEOUT_MS = 90_000;   // cart considered offline after
const SENSOR_STALE_MS = 5_000;        // no MQTT packet → sensor offline
const HISTORY_INTERVAL_MS = 5_000;    // cart logs a reading to Firestore

// Messages the doctor can push onto the cart screen (English + Bangla).
const PROMPTS = {
  finger: { en: 'Please place your fingertip on the sensor', bn: 'অনুগ্রহ করে সেন্সরে আঙুলের ডগা রাখুন' },
  still:  { en: 'Please stay still', bn: 'অনুগ্রহ করে স্থির থাকুন' },
  closer: { en: 'Please come closer to the camera', bn: 'অনুগ্রহ করে ক্যামেরার কাছে আসুন' },
  louder: { en: 'Please speak a little louder', bn: 'অনুগ্রহ করে একটু জোরে বলুন' },
  nurse:  { en: 'A nurse is on the way to help you', bn: 'আপনাকে সাহায্য করতে একজন নার্স আসছেন' },
  done:   { en: 'Thank you — we are done', bn: 'ধন্যবাদ — আমাদের কাজ শেষ' },
};

window.MC_CONFIG = Object.freeze({
  APP_VERSION,
  CART_ID,
  CART_LABEL,
  FIREBASE_CONFIG,
  MQTT,
  ICE_SERVERS,
  CART_VIDEO,
  CART_CAMERA_HINT,
  DOCTOR_VIDEO,
  RING_TIMEOUT_MS,
  DEFAULT_AUTO_ANSWER_S,
  PRESENCE_TIMEOUT_MS,
  SENSOR_STALE_MS,
  HISTORY_INTERVAL_MS,
  PROMPTS,
});
})();
