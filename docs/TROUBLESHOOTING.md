# Troubleshooting

Find what you see, then do what the right-hand column says. Start with **`medicart-status`** on the Pi: it checks the camera split, the robot program, the Arduino, the cart screen and the internet in one go.

- [Raspberry Pi](#raspberry-pi)
- [Arduino and driving](#arduino-and-driving)
- [Web console and cart screen](#web-console-and-cart-screen)
- [Video calls](#video-calls)
- [ESP32 sensor](#esp32-sensor)
- [Voice announcements](#voice-announcements)
- [Useful commands](#useful-commands)

## Raspberry Pi

| You see | Do |
|---|---|
| `medicart-status`: ✗ Arduino and ✗ camera | The Pi 5 supply is too weak, so USB is limited to 600 mA. Use a **5 V 5 A** supply. Then check the cables with `lsusb`. |
| Black screen after the boot logo | Use the HDMI port **next to the power socket**. |
| Ubuntu freezes or crashes during installation | Usually power or heat. Use the official supply and an active cooler; install with the desktop idle; `free -h` and `vcgencmd measure_temp` (or `cat /sys/class/thermal/thermal_zone0/temp`). |
| The cart screen doesn't open at power-on | Run `sudo bash ~/raspberry_pi/ubuntu-autostart.sh <screen-user>` again, then reboot. Check `KIOSK_URL` in `/etc/medicart/kiosk.conf`. |
| You want to leave the full-screen cart page | `Alt + F4`, or over SSH `pkill -f chromium`. It starts again at the next login. |
| Installer: *Hotspot password needs 8+ characters* | Run it again with a password of at least 8 characters. |
| Installer ran as the wrong user (SSH as another account) | `sudo env SUDO_USER=<screen-user> bash ~/raspberry_pi/install.sh` |
| Pasted command shows `^[[200~` | Open a new terminal, or run `bind 'set enable-bracketed-paste off'`, or `reset`. |
| `modprobe: FATAL: Module v4l2loopback not found` | Kernel headers don't match: `sudo apt full-upgrade`, reboot, run the installer again. |
| `medicart.py`: *Could not open camera 0* / device busy | The splitter owns the real camera. Use camera **10** (`MEDICART_VIDEO_SOURCE=10`). |
| `Dashboard off (No module named 'flask')` | `~/MideCart/venv/bin/pip install flask requests` |
| Pi hotspot never appears | `journalctl -u medicart-wifi-portal -b`; it needs NetworkManager. |
| Typo'd `systemctl` | It's `systemctl` (no `i`): `sudo systemctl status medicart-vision` |

## Arduino and driving

| You see | Do |
|---|---|
| Upload: *permission denied* on `/dev/ttyACM0` | `sudo usermod -aG dialout $USER`, then log out and in. Arduino IDE as a snap: `sudo snap connect arduino:raw-usb`. |
| Upload goes to the wrong board | **Tools → Board → Arduino Mega or Mega 2560** (not an ESP32). |
| Upload hangs | Close the Serial Monitor and anything else using the port; replug; hold RESET until *Uploading…*. |
| Console Drive page: **Controller offline** | `journalctl -u medicart-vision \| grep FIREBASE`. *sign-in failed* → run `sudo ./install.sh` again with the right cart password. *TOO_MANY_ATTEMPTS* → wait, the bridge backs off by itself. |
| Manual greyed out ("needs firmware 3.x") | Upload `arduino/medicart/medicart.ino` again. |
| The robot won't move in Manual | The camera says STOP (a person, or vision not running). The Pi dashboard shows why. |
| Auto doesn't follow the line | Serial Monitor `i`: middle sensor over the line must read `00100`. Check the height (3–8 mm), the orientation (S1 on the left) and the trimmers. |
| It steers away from the line | The array is mounted backwards (S1 on the right). |
| A wheel turns the wrong way | Swap that motor's two leads on the BTS7960, not the code. |
| The wheels never turn | R_EN / L_EN on the BTS7960 boards not at 5 V; motor battery; common ground. |

## Web console and cart screen

| You see | Do |
|---|---|
| Stuck loading / *MediCart couldn't start* | The message says what failed. Usually no internet (Firebase loads from `www.gstatic.com`). |
| Console says **Cart offline** | The cart screen isn't open or isn't signed in. On the Pi: is Chromium showing `/cart`? |
| A change on GitHub doesn't show | Wait for Vercel to say *Ready*, then `Ctrl + F5`. On the Pi: `pkill -f chromium` or hold the gear → *Reload screen*. |
| Sign-in: *Email or password is incorrect* | Create the account in Firebase → Authentication → Users. |
| Records: *Missing or insufficient permissions* | Publish `firebase/firestore.rules`. |

## Video calls

| You see | Do |
|---|---|
| Rings, answers, then **Connection failed** | The networks block direct video. Check `ICE_SERVERS` (TURN) in `config.js`. The stats chip shows `relay` when TURN is used. |
| Cart shows **No camera** | `systemctl status medicart-camera`; webcam unplugged or `v4l2loopback` not loaded. The cart picks the camera up again within ~15 s. |
| Wrong camera on the cart | Hold the gear → *Telemedicine camera* → **MediCart-Call**. |
| Doctor can't hear the patient | Cart settings → Microphone. In the console, check the speaker button isn't off. |
| Patient can't hear the doctor | Cart settings → Speaker → *Test speaker*; `wpctl status` on the Pi. |
| Chromium asks for camera/mic or can't see them (snap) | `sudo snap connect chromium:camera` and `sudo snap connect chromium:audio-record` |

## ESP32 sensor

| You see | Do |
|---|---|
| *This chip is ESP32, not ESP32-S3* | Board → **ESP32 Dev Module**. |
| Serial Monitor: **No data from oximeter** | Module TX → GPIO 16, RX → GPIO 17, common GND, module powered. The firmware also tries swapped wires and other baud rates, and says what it found. |
| SpO₂ wrong or stuck | `SPO2IDX 0` to learn the byte again, or `RAW` to look at the packets. |
| SpO₂ disappears when the finger moves | Expected: it holds the last good value for up to 10 s (`spo2Held`). Keep the finger still. |
| LED blinks slowly forever | It can't reach the broker: port **8883** must be open on the network. |
| **Sensor offline** but the LED is solid | The topic prefix differs between `config.js` and the sketch, or the broker was changed in the setup page → *Advanced*. |
| Can't join a campus network | The ESP32 joins **2.4 GHz WPA2-Personal** only; use a lab router or a phone hotspot. |

## Voice announcements

| You see | Do |
|---|---|
| No sound at all | Cart settings → Speaker → *Test voice*; *Voice announcements* must be **On**. |
| "Step aside" during a consultation | Fixed in web 3.5.0. Make sure the Pi has reloaded the new `cart.html` (`pkill -f chromium`). |
| *Take your items* doesn't play | The cart must be online. The Drive page says *Played on the cart ✓* when it has played. |

## Useful commands

```bash
medicart-status                              # is everything running?
journalctl -u medicart-vision -f             # robot program (WEB / ARDUINO / FIREBASE lines)
journalctl -u medicart-camera -f             # camera splitter
journalctl -u medicart-wifi-portal -f        # Wi-Fi setup
sudo systemctl restart medicart-vision       # after changing /etc/medicart/control.env
v4l2-ctl --list-devices                      # MediCart-Vision (/dev/video10), MediCart-Call (/dev/video11)
ls /dev/ttyACM* /dev/ttyUSB*                 # is the Arduino there?
sudo medicart-wifi-setup                     # open the Wi-Fi setup hotspot now
pkill -f chromium                            # reload the cart screen
```
