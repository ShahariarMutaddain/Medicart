// Test stub: a signed-in user is injected by the test as window.__TEST_USER__
const USER = () => window.__TEST_USER__ || null;
export function getAuth() { return { get currentUser() { return USER(); } }; }
export function onAuthStateChanged(auth, cb) { setTimeout(() => cb(USER()), 30); return () => {}; }
export async function signInWithEmailAndPassword() { return { user: USER() }; }
export async function signOut() {}
export async function sendPasswordResetEmail() {}
export async function updateProfile(u, p) { Object.assign(u, p); }
