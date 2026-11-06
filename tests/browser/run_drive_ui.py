"""Browser test: the real built console (index.html) and cart screen (cart.html) driving the
real control service and firmware 2.1 (simulated hardware), through a local Firebase stand-in.

Run:  python3 run_drive_ui.py  [--shots DIR]
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
SITE = ROOT
STUB = os.path.join(HERE, "stub")
LIBS = os.environ.get("MC_LIBS", "")       # node_modules with mqtt, chart.js (optional)
sys.path.insert(0, os.path.join(ROOT, "tests", "e2e"))
from run_e2e import Rig, check, wait_until, RESULTS  # noqa: E402

SHOTS = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None


def serve_site():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=SITE)
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
    rig = Rig("fwhost21")
    site = serve_site()
    site_url = f"http://127.0.0.1:{site.server_address[1]}"
    try:
        rig.start_daemon()
        wait_until(lambda: rig.state().get("manualCapable"), 6)
        rig.start_vision()
        rig.vision_cmd = "SAFE"
        wait_until(lambda: rig.motors() == (180, 180), 4)

        with sync_playwright() as p:
            b = p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--autoplay-policy=no-user-gesture-required"])
            doc = '{uid:"uid-doc", email:"doctor1@iub.edu.bd", displayName:"Dr Test"}'
            ctx = b.new_context(viewport={"width": 1366, "height": 900}, device_scale_factor=2 if SHOTS else 1)
            routes(ctx, rig.base, doc)
            page = ctx.new_page()
            errs = []
            page.on("pageerror", lambda e: errs.append(str(e)))
            page.goto(f"{site_url}/index.html#drive")
            check(page.wait_for_selector("#page-drive.on", timeout=8000) is not None, "console: Drive page opens from the nav route")
            page.wait_for_function("document.getElementById('drvCtlPill').textContent.includes('online')", timeout=8000)
            check(True, "console: shows 'Controller online' from the Pi's state")
            check(page.locator("#modeAuto").get_attribute("class").find("on") >= 0, "console: Auto highlighted while the robot line-follows")
            check("medicart" in page.locator("#drvFw").inner_text(), "console: firmware shown", page.locator("#drvFw").inner_text())
            check(page.locator("#drvLine i.on").count() == 1, "console: line sensors drawn (00100)")
            check(page.locator("#pad").get_attribute("class").find("locked") >= 0, "console: drive pad locked outside Manual")
            check("Drive: Auto" in page.locator("#rsDriveText").inner_text(), "console: side rail shows Drive: Auto")

            # Manual
            page.click("#modeManual")
            page.wait_for_function("document.getElementById('drvPadPill').textContent === 'Ready'", timeout=6000)
            check(wait_until(lambda: rig.motors() == (0, 0), 2), "console Manual: robot stops line following")
            check(page.locator("#navDrive").is_visible(), "console: MANUAL badge in the nav")

            # keyboard hold-to-drive
            page.locator("#pad").focus()
            page.keyboard.down("KeyW")
            moving = wait_until(lambda: rig.motors() == (120, 120), 3)
            check(moving, "keyboard: hold W → wheels 120/120 (speed slider default)", rig.motors())
            time.sleep(0.6)
            if SHOTS:
                page.screenshot(path=os.path.join(SHOTS, "drive-manual-desktop.png"))
            check(page.locator("#drvPadPill").inner_text() == "Driving", "console: pad shows Driving")
            page.keyboard.up("KeyW")
            t0 = time.time()
            check(wait_until(lambda: rig.motors() == (0, 0), 2) and time.time() - t0 < 1.2, "keyboard: release W → wheels stop", f"{time.time() - t0:.2f}s")

            # pointer joystick: drag right on the middle line → pivot right
            box = page.locator("#pad").bounding_box()
            cx, cy = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
            page.mouse.move(cx, cy)
            page.mouse.down()
            page.mouse.move(cx + box["width"] * 0.4, cy, steps=5)
            pivot = wait_until(lambda: rig.motors()[0] > 50 and rig.motors()[1] < -50, 3)
            check(pivot, "joystick: drag right → pivot on the spot", rig.motors())
            page.mouse.move(cx, cy - box["height"] * 0.4, steps=5)
            fwd = wait_until(lambda: rig.motors()[0] > 80 and rig.motors()[1] > 80, 3)
            check(fwd, "joystick: drag up → forward", rig.motors())
            page.mouse.up()
            check(wait_until(lambda: rig.motors() == (0, 0), 2), "joystick: let go → stop")

            # person ahead
            rig.vision_cmd = "STOP"
            time.sleep(0.6)
            page.keyboard.down("KeyW")
            time.sleep(1.0)
            check(rig.motors() == (0, 0), "person ahead: W does not move the cart forward")
            page.wait_for_function("!document.getElementById('drvAlert').hidden", timeout=4000)
            check("Person ahead" in page.locator("#drvAlert").inner_text(), "console: 'Person ahead' warning shown")
            if SHOTS:
                page.screenshot(path=os.path.join(SHOTS, "drive-blocked.png"))
            page.keyboard.up("KeyW")
            rig.vision_cmd = "SAFE"
            time.sleep(1.2)

            # cart screen banner while in manual
            cctx = b.new_context(viewport={"width": 800, "height": 480}, device_scale_factor=2 if SHOTS else 1)
            routes(cctx, rig.base, '{uid:"uid-cart", email:"cart01@medicart.iub", displayName:"MediCart"}')
            cart = cctx.new_page()
            cerrs = []
            cart.on("pageerror", lambda e: cerrs.append(str(e)))
            cart.goto(f"{site_url}/cart.html")
            ok = cart.wait_for_function("!document.getElementById('kDrive').hidden", timeout=10000)
            check(ok is not None and "Manual driving" in cart.locator("#kDrive").inner_text(), "cart screen: 'Manual driving' banner (EN + BN)")
            if SHOTS:
                page.keyboard.down("KeyW"); time.sleep(1.0)
                cart.screenshot(path=os.path.join(SHOTS, "cart-manual-banner.png"))
                page.keyboard.up("KeyW")

            # E-STOP
            page.click("#estopBtn")
            page.wait_for_function("document.getElementById('drvModePill').textContent === 'EMERGENCY STOP'", timeout=5000)
            check(True, "console: E-STOP button → EMERGENCY STOP shown")
            check(wait_until(lambda: rig.motors() == (0, 0), 1), "E-STOP: wheels still")
            check("latched" in page.locator("#estopBtn").get_attribute("class"), "console: E-STOP button pulses while latched")
            cart.wait_for_function("document.getElementById('kDriveEn').textContent === 'Emergency stop'", timeout=5000)
            check(True, "cart screen: 'Emergency stop' banner")
            if SHOTS:
                cart.screenshot(path=os.path.join(SHOTS, "cart-estop-banner.png"))
                page.screenshot(path=os.path.join(SHOTS, "drive-estop.png"))

            # back to Auto
            page.click("#modeAuto")
            check(wait_until(lambda: rig.motors() == (180, 180), 4), "console Auto: line following resumes")
            cart.wait_for_function("document.getElementById('kDrive').hidden", timeout=5000)
            check(True, "cart screen: banner hidden in Auto")
            if SHOTS:
                page.screenshot(path=os.path.join(SHOTS, "drive-auto.png"))

            # phone layout
            ph = b.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=3 if SHOTS else 1, has_touch=True, is_mobile=True)
            routes(ph, rig.base, doc)
            mob = ph.new_page()
            mob.goto(f"{site_url}/index.html#drive")
            mob.wait_for_function("document.getElementById('drvCtlPill').textContent.includes('online')", timeout=8000)
            overflow = mob.evaluate("document.documentElement.scrollWidth > innerWidth + 1")
            check(not overflow, "phone 390 px: Drive page has no sideways scroll")
            if SHOTS:
                mob.screenshot(path=os.path.join(SHOTS, "drive-phone.png"), full_page=True)

            check(not errs, "console: no script errors", "; ".join(errs[:3]))
            check(not cerrs, "cart screen: no script errors", "; ".join(cerrs[:3]))
            b.close()
    finally:
        rig.close()
        site.shutdown()
    bad = [w for ok, w in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(bad)}/{len(RESULTS)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
