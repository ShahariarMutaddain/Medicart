# Contributing to MediCart

Thanks for helping. MediCart moves around people, so a few rules matter more here than in most projects.

## Ground rules

1. **Don't change the team's LFR code.** `arduino/medicart_lfr_original.ino.txt` is what ran on the robot, and firmware 3.4 must keep producing the same PWM. The firmware test checks this.
2. **Don't remove Rifat's work.** His originals (`*.original`, `*.rifat.py`, `arduino/history/`) stay. Add on top and mark your changes.
3. **The browser never drives the motors directly.** Web pages write requests; `medicart.py` decides.
4. **Never commit secrets.** No passwords, `.env` files or `control.env`. Device passwords live on the devices (`/etc/medicart/`, ESP32 flash).
5. **Safety changes need a robot test.** Wheels lifted first, then a clear corridor: let-go stop, person stop in Auto and Manual, emergency stop, USB unplugged while driving, stopping distance.

## Workflow

```bash
git clone https://github.com/tahsanmasum/MediCart.git && cd MediCart
git checkout -b my-change
# … edit …
python3 tools/build_pages.py        # if you changed web-src/
bash tests/run_all.sh               # must pass
git commit -m "area: what changed" && git push -u origin my-change
```

Then open a pull request. The template lists the checks.

| You changed | Also do |
|---|---|
| `web-src/**` | run `tools/build_pages.py` and commit `index.html` + `cart.html` |
| `arduino/medicart/medicart.ino` | run the firmware tests; bump the version string in `FW medicart x.y` and `STAT` |
| `esp32_sensor/**` | run `bash tests/esp32/run.sh`; rebuild `prebuilt/` if you change behaviour |
| a protocol, path or setting | update [`docs/PROTOCOLS.md`](docs/PROTOCOLS.md) |
| a pin | update [`docs/HARDWARE.md`](docs/HARDWARE.md) and the hardware reference |

## Commit messages

`area: short summary`, for example `firmware: cap DRIVE at 180`, `web: quiet voice during calls`, `docs: ESP32 wiring`.
