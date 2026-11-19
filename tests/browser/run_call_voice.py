"""Browser test: a doctor's video call to the cart plays 0008 ("the doctor is ready") before the
cart answers by itself, and the call connects. Uses the same fake Firebase rig as run_ui_v3.py."""
import os
import sys
import time

from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run_ui_v3 import routes, serve_site, AUDIO_RECORDER, ROOT, check, RESULTS  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tests", "e2e"))
from rig3 import Rig3, wait_until  # noqa: E402


def main():
    rig = Rig3(os.path.join(ROOT, "tests", "e2e", "fwhost3"))
    site = serve_site()
    url = f"http://127.0.0.1:{site.server_address[1]}"
    try:
        rig.start_robot()
        with sync_playwright() as p:
            b = p.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
            cctx = b.new_context(viewport={"width": 800, "height": 480})
            routes(cctx, rig.base, '{uid:"uid-cart", email:"cart01@medicart.iub", displayName:"MediCart"}')
            cctx.add_init_script(AUDIO_RECORDER.replace("300);", "3400);"))   # 0008 lasts about 3.4 s
            cart = cctx.new_page()
            cerrs = []
            cart.on("pageerror", lambda e: cerrs.append(str(e)))
            cart.goto(f"{url}/cart.html")
            time.sleep(3)
            dctx = b.new_context(viewport={"width": 1366, "height": 900})
            routes(dctx, rig.base, '{uid:"uid-doc", email:"doctor1@iub.edu.bd", displayName:"Dr Test"}')
            doc = dctx.new_page()
            derrs = []
            doc.on("pageerror", lambda e: derrs.append(str(e)))
            doc.goto(f"{url}/index.html#consult")
            doc.wait_for_function("!document.getElementById('callBtn').disabled", timeout=15000)
            doc.click("#callBtn")
            cart.wait_for_function("!document.getElementById('incoming').hidden", timeout=10000)
            check(True, "cart rings")
            t_ring = time.time()
            played = lambda: cart.evaluate("window.__played.slice()")
            check(wait_until(lambda: "0008" in played(), 3), "ringing -> 0008 'the doctor is ready to talk with you'", played())
            cart.wait_for_function("!document.getElementById('call').hidden", timeout=20000)
            waited = time.time() - t_ring
            check(waited >= 3.3, "the cart answers only after the announcement has finished", f"{waited:.1f}s")
            # ── during the consultation: no automatic announcements ──
            import json as _json
            emit = lambda d: cart.evaluate("(m) => window.__mqttEmit('iub-medicart-591a7e01d2/01/sensors', m)", d)
            n0 = len(played())
            rig.mode("auto")                      # the cart was left in Auto ...
            rig.person = "close"                  # ... and the patient sits in front of the camera
            wait_until(lambda: rig.state().get("vision") == "blocked", 6)
            emit(_json.dumps({"heartRate": 0, "spo2": 0, "fingerDetected": True, "state": "measuring"}))
            time.sleep(1)
            emit(_json.dumps({"heartRate": 80, "spo2": 98, "fingerDetected": True, "state": "locked", "samplesCollected": 4}))
            time.sleep(12)
            check(played()[n0:] == [], "in the consultation: patient in front (Auto, blocked) + vitals -> no voice at all", played()[n0:])
            doc.click("#endBtn")                 # the doctor ends the consultation
            cart.wait_for_function("document.getElementById('call').hidden", timeout=10000)
            time.sleep(12)
            check(played()[n0:] == [], "right after the consultation (patient still there): still quiet", played()[n0:])
            rig.person = None
            check(not cerrs and not derrs, "no page errors", (cerrs + derrs)[:2])
            b.close()
    finally:
        rig.close()
        site.shutdown()
    bad = [w for ok, w in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(bad)}/{len(RESULTS)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
