#!/usr/bin/env python3
"""Build the two self-contained website pages from web-src/.

  web-src/index.html + js/ + assets/  ->  index.html   (doctor console)
  web-src/cart.html  + js/ + assets/  ->  cart.html    (cart screen)

Styles, logo, avatars, settings (js/config.js) and app code are put inside each page,
so Vercel only needs index.html, cart.html and vercel.json.

Edit files in web-src/, then run from the repo root:   python3 tools/build_pages.py
"""
import base64
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "web-src"


def datauri(rel: str) -> str:
    p = SRC / rel
    mime = "image/png" if p.suffix == ".png" else "image/svg+xml"
    return f"data:{mime};base64," + base64.b64encode(p.read_bytes()).decode()


def build(page: str) -> int:
    s = (SRC / page).read_text(encoding="utf-8")
    s = re.sub(r'<link rel="stylesheet" href="(assets/[^"]+\.css)">',
               lambda m: f"<style>/* {m.group(1)} */\n" + (SRC / m.group(1)).read_text(encoding="utf-8") + "\n</style>", s)
    s = re.sub(r'<script src="(js/[^"]+\.js)"></script>',
               lambda m: f"<script>/* ═══ {m.group(1)} ═══ */\n" + (SRC / m.group(1)).read_text(encoding="utf-8") + "\n</script>", s)
    m = re.search(r'<script src="(js/[^"]+\.js)" defer></script>\n', s)
    if m:  # the app script runs after the markup, right before the Firebase loader (same order as defer)
        app = m.group(1)
        s = s.replace(m.group(0), "")
        i = s.index('<script type="module">')
        s = s[:i] + f"<script>/* ═══ {app} (after the page markup, like defer) ═══ */\n" + \
            (SRC / app).read_text(encoding="utf-8") + "\n</script>\n" + s[i:]
    s = re.sub(r"assets/(iub-logo\.png|avatars/[a-z]+\.svg)", lambda m: datauri("assets/" + m.group(1)), s)
    s = s.replace("Make sure the js/ folder sits next to this page.", "Reload the page.")
    for bad in ('src="js/', 'href="assets/', 'src="assets/'):
        if bad in s:
            sys.exit(f"{page}: unresolved reference {bad}")
    (ROOT / page).write_text(s, encoding="utf-8")
    return len(s)


if __name__ == "__main__":
    for page in ("index.html", "cart.html"):
        print(f"{page}: {build(page) // 1024} KB")
