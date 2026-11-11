"""Browser test, MediCart 3.0: the built Vercel console (Drive page), the cart screen banner and
Rifat's Pi dashboard page, all driving the REAL medicart.py + firmware 3.0 (simulated hardware).

Run:  python3 run_ui_v3.py [--shots DIR]
"""
import functools
import http.server
import os
import sys
import threading
import time

from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
STUB = os.path.join(HERE, "stub")
LIBS = os.environ.get("MC_LIBS", "")
sys.path.insert(0, os.path.join(ROOT, "tests", "e2e"))
from rig3 import Rig3, wait_until  # noqa: E402

SHOTS = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
RESULTS = []


def check(cond, what, detail=""):
    RESULTS.append((bool(cond), what))
    print(("PASS " if cond else "FAIL ") + what + (f"   [{detail}]" if detail != "" else ""), flush=True)


def serve_site():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=ROOT)
    handler.log_message = lambda *a: None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def routes(ctx, fb_base, user):
    def js(path):
        return lambda route: route.fulfill(content_type="application/javascript", body=open(path).read())
    for name in ("firebase-app", "firebase-auth", "firebase-database", "firebase-firestore"):
        ctx.route(f"**/firebasejs/10.12.2/{name}.js", js(os.path.join(STUB, f"{name}.js")))
    if LIBS:
        ctx.route("**/unpkg.com/mqtt*/**", js(os.path.join(LIBS, "mqtt/dist/mqtt.min.js")))
        ctx.route("**/cdnjs.cloudflare.com/**/chart*", js(os.path.join(LIBS, "chart.js/dist/chart.umd.js")))
    else:
        ctx.route("**/unpkg.com/**", lambda r: r.fulfill(content_type="application/javascript", body="window.mqtt={connect(){return{on(){},end(){},subscribe(){}}}}"))
        ctx.route("**/cdnjs.cloudflare.com/**", lambda r: r.fulfill(content_type="application/javascript", body="window.Chart=function(){return{update(){},resize(){},destroy(){},data:{datasets:[{data:[]}],labels:[]},options:{}}}"))
    ctx.route("**/fonts.googleapis.com/**", lambda r: r.fulfill(content_type="text/css", body=""))
    ctx.route("**/fonts.gstatic.com/**", lambda r: r.abort())
    ctx.route("**/broker.hivemq.com/**", lambda r: r.abort())
    ctx.add_init_script(f"window.__TEST_FB__ = {fb_base!r}; window.__TEST_USER__ = {user};")


def main():
    rig = Rig3(os.path.join(ROOT, "tests", "e2e", "fwhost3"))
    site = serve_site()
    site_url = f"http://127.0.0.1:{site.server_address[1]}"
    try:
        rig.start_robot()
        wait_until(lambda: rig.state().get("manualCapable"), 15)
        with sync_playwright() as p:
            b = p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
            dsf = 2 if SHOTS else 1
            doc = '{uid:"uid-doc", email:"doctor1@iub.edu.bd", displayName:"Dr Test"}'

            # ── Vercel console: Drive page ──
            ctx = b.new_context(viewport={"width": 1366, "height": 900}, device_scale_factor=dsf)
            routes(ctx, rig.base, doc)
            page = ctx.new_page()
            errs = []
            page.on("pageerror", lambda e: errs.append(str(e)))
            page.goto(f"{site_url}/index.html#drive")
            page.wait_for_function("document.getElementById('drvCtlPill').textContent.includes('online')", timeout=10000)
            check(True, "console: Controller online (state from medicart.py via Firebase)")
            check(page.locator("#drvModePill").inner_text() == "STOPPED", "console: robot starts STOPPED", page.locator("#drvModePill").inner_text())
            check("3.1" in page.locator("#drvFw").inner_text(), "console: firmware 3.1 shown")
            page.click("#modeAuto")
            check(wait_until(lambda: rig.motors() == (38, 38), 6), "console Auto -> line following", rig.motors())
            page.click("#modeManual")
            page.wait_for_function("document.getElementById('drvPadPill').textContent === 'Ready'", timeout=8000)
            check(wait_until(lambda: rig.motors() == (0, 0), 3), "console Manual -> line following stops")
            page.locator("#pad").focus()
            page.keyboard.down("KeyW")
            check(wait_until(lambda: rig.motors() == (40, 40), 4), "console: hold W -> 40/40", rig.motors())
            time.sleep(0.6)
            if SHOTS:
                page.screenshot(path=os.path.join(SHOTS, "console-drive-manual.png"))
            page.keyboard.up("KeyW")
            t0 = time.time()
            check(wait_until(lambda: rig.motors() == (0, 0), 3), "console: release W -> stop", f"{time.time() - t0:.2f}s")
            rig.person = "close"
            page.keyboard.down("KeyW")
            time.sleep(1.2)
            check(rig.motors() == (0, 0), "console: person in the stop zone -> no movement")
            page.wait_for_function("!document.getElementById('drvAlert').hidden", timeout=5000)
            check("camera says STOP" in page.locator("#drvAlert").inner_text(), "console: camera STOP warning shown")
            page.keyboard.up("KeyW")
            rig.person = None

            # ── cart screen banner ──
            cctx = b.new_context(viewport={"width": 800, "height": 480}, device_scale_factor=dsf)
            routes(cctx, rig.base, '{uid:"uid-cart", email:"cart01@medicart.iub", displayName:"MediCart"}')
            cart = cctx.new_page()
            cerrs = []
            cart.on("pageerror", lambda e: cerrs.append(str(e)))
            cart.goto(f"{site_url}/cart.html")
            cart.wait_for_function("!document.getElementById('kDrive').hidden", timeout=12000)
            check("Manual driving" in cart.locator("#kDrive").inner_text(), "cart screen: Manual driving banner")
            page.click("#estopBtn")
            check(wait_until(lambda: rig.motors() == (0, 0) and rig.status()["mode"] == "stopped", 4), "console EMERGENCY STOP")
            cart.wait_for_function("document.getElementById('kDriveEn').textContent === 'Emergency stop'", timeout=6000)
            check(True, "cart screen: Emergency stop banner")
            if SHOTS:
                cart.screenshot(path=os.path.join(SHOTS, "cart-estop.png"))

            # ── Rifat's Pi dashboard page ──
            dctx = b.new_context(viewport={"width": 1366, "height": 860}, device_scale_factor=dsf)
            dash = dctx.new_page()
            derrs = []
            dash.on("pageerror", lambda e: derrs.append(str(e)))
            dash.goto(rig.dash + "/")
            dash.wait_for_function("document.getElementById('pMode').textContent === 'STOPPED'", timeout=8000)
            check(True, "Pi dashboard: loads, shows STOPPED")
            dash.wait_for_function("document.getElementById('video').naturalWidth > 0", timeout=8000)
            check(True, "Pi dashboard: live camera stream shows")
            dash.click("#bManual")
            check(wait_until(lambda: rig.status()["mode"] == "manual", 4), "Pi dashboard: Manual")
            dash.locator("#pad").focus()
            dash.keyboard.down("KeyW")
            check(wait_until(lambda: rig.motors() == (40, 40), 4), "Pi dashboard: hold W -> 40/40", rig.motors())
            time.sleep(0.6)
            if SHOTS:
                dash.screenshot(path=os.path.join(SHOTS, "pi-dashboard-manual.png"))
            dash.keyboard.up("KeyW")
            check(wait_until(lambda: rig.motors() == (0, 0), 2), "Pi dashboard: release -> stop")
            box = dash.locator("#pad").bounding_box()
            cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
            dash.mouse.move(cx, cy); dash.mouse.down(); dash.mouse.move(cx + box["width"] * 0.4, cy, steps=4)
            check(wait_until(lambda: rig.motors()[0] > 20 and rig.motors()[1] < -20, 3), "Pi dashboard: drag right -> turn", rig.motors())
            dash.mouse.up()
            wait_until(lambda: rig.motors() == (0, 0), 2)
            check(wait_until(lambda: rig.state().get("mode") == "MANUAL", 4), "console follows a mode set on the Pi dashboard")
            dash.keyboard.press("Escape")
            check(wait_until(lambda: rig.status()["mode"] == "stopped", 3), "Pi dashboard: Esc = EMERGENCY STOP")
            dash.click("#bAuto")
            check(wait_until(lambda: rig.motors() == (38, 38), 4), "Pi dashboard: Auto -> line following")
            if SHOTS:
                time.sleep(0.5)
                dash.screenshot(path=os.path.join(SHOTS, "pi-dashboard-auto.png"))
            ph = b.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=3 if SHOTS else 1, is_mobile=True, has_touch=True)
            mob = ph.new_page()
            mob.goto(rig.dash + "/")
            mob.wait_for_function("document.getElementById('pMode').textContent !== '—'", timeout=8000)
            check(not mob.evaluate("document.documentElement.scrollWidth > innerWidth + 1"), "Pi dashboard on a phone: no sideways scroll")
            if SHOTS:
                mob.screenshot(path=os.path.join(SHOTS, "pi-dashboard-phone.png"), full_page=True)
            rig.api("/api/stop", {"name": "test end"})
            check(not errs, "console: no script errors", "; ".join(errs[:2]))
            check(not cerrs, "cart screen: no script errors", "; ".join(cerrs[:2]))
            check(not derrs, "Pi dashboard: no script errors", "; ".join(derrs[:2]))
            b.close()
    finally:
        rig.close()
        site.shutdown()
    bad = [w for ok, w in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(bad)}/{len(RESULTS)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
