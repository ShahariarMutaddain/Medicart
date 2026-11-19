# Security

## Reporting

If you find a way to make the cart move when it shouldn't, to bypass the stop rules, or to reach patient data, **don't open a public issue**. Contact the maintainers privately (GitHub → *Security* → *Report a vulnerability*, or the team directly). Safety issues come first.

## What is and isn't secret

| Item | Where | Secret? |
|---|---|---|
| Firebase web config (`apiKey`, project ID) | `web-src/js/config.js`, the web pages | **No.** Public by design. Protection = security rules + sign-up disabled. |
| Doctor and cart account passwords | Firebase Authentication; the cart's in `/etc/medicart/control.env` (root-only) | **Yes.** Never in the repository. |
| Pi dashboard, Wi-Fi portal and hotspot passwords | `/etc/medicart/` on the Pi | **Yes** |
| ESP32 admin and Wi-Fi passwords | the ESP32's flash | **Yes** |
| MQTT topic prefix | `config.js`, the ESP32 sketch, the web pages | **Semi.** It's the only lock on the public broker. Anyone who reads the pages can see it. |
| TURN credentials | `config.js`, the web pages | **Semi.** Visible in the browser by design. Rotate them in Metered if they're abused. |

> [!IMPORTANT]
> If this repository is **public**, the MQTT prefix and the TURN credentials in `config.js` are public too (they already are in the live pages). For real patient data, use a **private MQTT broker with a login**, restrict the TURN credential, and keep the repository private.

## Checklist for a new deployment

- [ ] Firebase sign-up disabled; Realtime Database and Firestore rules published
- [ ] Strong cart account password, typed only on the cart and in the installer
- [ ] ESP32 admin password changed from `medicart-admin`
- [ ] Pi Wi-Fi portal admin and hotspot passwords changed from the defaults
- [ ] Pi dashboard password set (`DASHBOARD_PASSWORD`)
- [ ] New MQTT topic prefix (in both `config.js` and the sketch) if the old one leaked

Video and audio go peer-to-peer between the doctor's browser and the cart (WebRTC, DTLS-SRTP). Firebase carries only call setup, presence, readings and notes.
