# Firebase rules

| File | Publish to |
|---|---|
| [`database.rules.json`](database.rules.json) | Firebase console → **Realtime Database → Rules** |
| [`firestore.rules`](firestore.rules) | Firebase console → **Firestore Database → Rules** |
| [`firebase.json`](firebase.json), [`.firebaserc`](.firebaserc) | for the command line: `npm i -g firebase-tools && firebase login && cd firebase && firebase deploy --only database,firestore:rules` |

Every path needs a signed-in user. Also turn **off** self sign-up (Authentication → Settings → User actions → untick *Enable create*). The Firebase web config is public by design, so these rules and disabled sign-up are what protect the data.

Accounts to create (Authentication → Users → Add user):

- one per **doctor** (their email + a password);
- one **cart account**, e.g. `cart01@medicart.iub`. The email doesn't need to be real. It's used by the cart screen and by `firebase_bridge.py` on the Pi.

What lives where: [`docs/PROTOCOLS.md`](../docs/PROTOCOLS.md#5-firebase-realtime-database).
