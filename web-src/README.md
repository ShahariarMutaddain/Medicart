# Website source

The doctor console and the cart screen. Edit here, then build: the two pages Vercel serves (`/index.html`, `/cart.html` at the repository root) are generated from this folder.

| Path | What it is |
|---|---|
| [`index.html`](index.html) · [`js/console.js`](js/console.js) · [`assets/console.css`](assets/console.css) | **Doctor console**: sign-in, Overview, Vitals, Consult, Records, Drive, Data stream, System |
| [`cart.html`](cart.html) · [`js/cart.js`](js/cart.js) · [`assets/cart.css`](assets/cart.css) | **Cart screen**: patient kiosk in English + Bangla, call answering, voice announcements, settings (hold the gear) |
| [`js/config.js`](js/config.js) | **The settings**: Firebase project, cart ID, MQTT broker + topic prefix, TURN servers, camera sizes, `APP_VERSION` |
| [`js/shared.js`](js/shared.js) | shared helpers: MQTT client, sensor normalisation, formatting |
| [`assets/brand.css`](assets/brand.css) | IUB colours (navy `#0A2647`, gold `#C8861B`, plum `#5B2C8E`), type, components |
| [`assets/avatars/`](assets/avatars/) | doctor, nurse, patient and MediCart illustrations |
| [`assets/sounds/`](assets/sounds/) | Bangla voice announcements `0001`–`0010` |
| [`assets/iub-logo.png`](assets/iub-logo.png) | IUB seal |

## Build

```bash
python3 tools/build_pages.py
```

[`tools/build_pages.py`](../tools/build_pages.py) inlines the CSS, JS, images and mp3 files into two self-contained pages, `index.html` and `cart.html` at the repository root (`cart.html` is about 1 MB because of the sounds). Commit those two files; Vercel serves them as they are.

## Try it locally

```bash
python3 -m http.server 8000      # from the repository root
# console: http://localhost:8000/        cart screen: http://localhost:8000/cart.html
```

It talks to the real Firebase project in `config.js`. Use **Demo data** in the console's top bar to simulate the sensor.

## Libraries (loaded from CDNs)

Firebase JS SDK 10.12.2 (gstatic) · MQTT.js 5.3.5 (unpkg) · Chart.js 4.4.4 (cdnjs, console only) · Google Fonts: Fraunces, DM Sans, DM Mono, Noto Sans Bengali.

## Replacing a sound

Put the new recording at `assets/sounds/00NN.mp3` (same number), run the build and push `cart.html`. When each sound plays is listed in [`docs/PROTOCOLS.md`](../docs/PROTOCOLS.md#7-voice-announcements).
