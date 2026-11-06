export const getFirestore = () => ({});
export const collection = (db, n) => ({ n });
export const query = (c) => c;
export const orderBy = () => ({});
export const limit = () => ({});
export async function addDoc() { return { id: 'x' }; }
export async function getDocs() { return { docs: [], size: 0, empty: true, forEach() {} }; }
export function writeBatch() { return { delete() {}, set() {}, commit: async () => {} }; }
export const serverTimestamp = () => new Date();
export const Timestamp = { fromDate: (d) => ({ toDate: () => d, toMillis: () => d.getTime() }), now: () => ({ toDate: () => new Date(), toMillis: () => Date.now() }) };
