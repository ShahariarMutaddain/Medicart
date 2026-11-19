# Raspberry Pi setup

Everything that turns a Raspberry Pi 5 into the cart's brain and makes it **plug and play**: power on and the cart screen, the camera split, the robot program and the web link all start by themselves.

| Path | What it is |
|---|---|
| [`install.sh`](install.sh) | **Run this.** Asks a few questions, installs packages and every service below. Safe to run again. |
| [`ubuntu-autostart.sh`](ubuntu-autostart.sh) | Ubuntu Desktop (GNOME) plug and play: auto-login on X11, no lock screen or pop-ups, snap Chromium with camera + mic, cart screen at login, `medicart-status`. `install.sh` runs it on Ubuntu. |
| [`camera/medicart-camera.sh`](camera/medicart-camera.sh) | ffmpeg splitter: one USB camera → `/dev/video10` *MediCart-Vision* + `/dev/video11` *MediCart-Call* |
| [`kiosk/start-kiosk.sh`](kiosk/start-kiosk.sh) | Chromium full screen on the `/cart` page, restarts it if it closes |
| [`vision/run-vision.sh`](vision/run-vision.sh) | starts `medicart.py` with the right Python and camera 10 |
| [`control/medicart-drive`](control/medicart-drive) | keyboard driving over SSH |
| [`wifi-portal/medicart-wifi-portal.py`](wifi-portal/medicart-wifi-portal.py) | opens the `MediCart-Cart-Setup` hotspot + setup page when no known Wi-Fi is found |
| [`systemd/`](systemd/) | `medicart-camera`, `medicart-vision`, `medicart-wifi-portal` services |
| [`config/*.example`](config/) | templates for `/etc/medicart/*.conf` and `control.env` |

## Install

```bash
# once: libraries for the robot program
cd ~/MideCart && python3 -m venv venv && venv/bin/pip install ultralytics pyserial flask requests
sudo usermod -aG dialout,video,audio $USER && sudo reboot

# then: MediCart itself
cd ~/raspberry_pi && chmod +x install.sh && sudo ./install.sh
```

| It asks | Answer |
|---|---|
| Cart page URL | `https://<your-project>.vercel.app/cart` |
| Admin password for the Wi-Fi setup page | a new password |
| Password of the setup hotspot | another one, 8+ characters |
| Path to `medicart.py` | `/home/<user>/MideCart/vision/medicart.py` |
| Set up driving control? | `y` |
| Cart account email / password | the cart account from Firebase |
| Firebase apiKey / databaseURL | Enter (defaults) |
| Pi dashboard password | a new password |
| Python for `medicart.py` | `/home/<user>/MideCart/venv/bin/python` |

Logged in over SSH as a different user than the one on the screen? Tell the installer who the screen user is:

```bash
sudo env SUDO_USER=<screen-user> bash ~/raspberry_pi/install.sh
sudo bash ~/raspberry_pi/ubuntu-autostart.sh <screen-user>
```

Non-interactive: `sudo KIOSK_URL=… ADMIN_PASSWORD=… VISION_SCRIPT=… ./install.sh --yes`

## Check

```bash
medicart-status
```

```text
✓ Camera split (video10 + video11)
✓ Robot program (medicart-vision)
✓ Arduino connected
✓ Cart screen (Chromium kiosk)
✓ Internet
── last robot program messages:
[ARDUINO] FW medicart 3.4
[WEB] Dashboard available at http://0.0.0.0:5000
[FIREBASE] signed in as cart01@medicart.iub
```

| Command | Does |
|---|---|
| `journalctl -u medicart-vision -f` | robot program log |
| `sudo systemctl restart medicart-vision` | after editing `/etc/medicart/control.env` |
| `sudo medicart-wifi-setup` | open the Wi-Fi setup hotspot now |
| `medicart-drive` | drive from the keyboard (SSH) |
| `pkill -f chromium` | reload the cart screen |

Nothing secret is stored in this folder. Passwords go into `/etc/medicart/` on the Pi (`control.env` is root-only).
