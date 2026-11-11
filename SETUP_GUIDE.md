# MediCart 3.1 — setup, step by step (PC + Raspberry Pi with Ubuntu)

Do one step, check the ✅, then go on. Keep the robot's **wheels lifted** until step 40.

## A. Arduino (from your PC)
1. Unzip `medicart-v3.zip`. Open the `medicart-system` folder.
2. Arduino IDE → **File → Open** → `arduino/medicart/medicart.ino`.
3. **Tools → Board → Arduino Mega or Mega 2560**, **Port →** the Mega → **Upload**. ✅ *Done uploading*.
   - Upload error? Close the Serial Monitor and VS Code, replug the USB cable, and hold RESET until it says *Uploading…*.
4. **Serial Monitor**: 115200. ✅ `MediCart ready`, `FW medicart 3.1`, then `VIEW AUTO STOPPED … S1: 0 S2: 0 … | PWM_L: 0 PWM_R: 0` lines.
5. Type `w` + Enter. ✅ Forward for 1 s at PWM 38. Then try `a` (left), `s` (back), `d` (right), `x` (stop).
   - A wheel turns the wrong way? Type `TESTL` / `TESTR`, then `FLIPL` / `FLIPR` / `SWAP`.
6. **IR array:** put the robot on the floor and watch `S1 … S5`. ✅ Line under the middle sensor: `S3: 1`, all others `0`.
7. Type `l` (on the floor, someone next to the robot). ✅ It follows the line like your old LFR code (same speed 38, same turns) for 60 s, printing `forwrd`, `slide left …`. Type `x` to stop.
8. Close the Serial Monitor and unplug the Mega from the PC.

## B. Pi memory card
9. Raspberry Pi Imager → your Pi → **Other general-purpose OS → Ubuntu → Ubuntu Desktop 24.04 LTS (64-bit)** → your card → **Next → No → Yes**. Windows asks to *format*? **Cancel**.
10. Copy `MideCart` and `raspberry_pi` from `medicart-system` onto a USB stick.

## C. Pi first start
11. Card in the Pi. Monitor on the HDMI port **next to the power socket**. Plug in the keyboard, mouse, webcam, speaker and **the Mega**, then the power last.
12. Setup screen: English, your Wi-Fi, time zone Dhaka, username **`medicart`**, a password.
13. Terminal (**Ctrl + Alt + T**). Paste with **Ctrl + Shift + V**:
    ```
    sudo apt update && sudo apt full-upgrade -y
    sudo apt install -y openssh-server python3-venv python3-pip git linux-headers-raspi
    sudo apt remove -y brltty
    sudo reboot
    ```

## D. Copy the files and install the robot program
14. USB stick in the Pi, then:
    ```
    ls /media/$USER/
    ```
    (shows the stick name, e.g. `KINGSTON`)
15. Copy the files (change `KINGSTON` to your stick's name):
    ```
    cp -r /media/$USER/KINGSTON/MideCart ~/
    cp -r /media/$USER/KINGSTON/raspberry_pi ~/
    ```
16. Install the libraries (10–25 min):
    ```
    cd ~/MideCart && python3 -m venv venv && venv/bin/pip install ultralytics pyserial flask requests
    ```
17. Give your user access to the Arduino, camera and sound:
    ```
    sudo usermod -aG dialout,video,audio $USER && sudo reboot
    ```
18. Test the robot program by hand:
    ```
    cd ~/MideCart/vision && ~/MideCart/venv/bin/python medicart.py
    ```
    ✅ It prints `[WEB] Dashboard available at http://127.0.0.1:5000` and `[ARDUINO] FW medicart 3.1`.
19. In the Pi's browser, open **http://127.0.0.1:5000**. ✅ You see the camera with detection boxes, and **STOPPED**.
20. Click **Manual**, then hold **W**. ✅ The wheels turn (speed 40; change it with the slider). Let go. ✅ They stop.
21. Click **Auto** (line under the sensors, nobody in front). ✅ The wheels turn. Step in front of the camera. ✅ They stop.
22. Press **Ctrl + C** in the Terminal. ✅ The wheels stop.

## E. Install MediCart (starts everything at boot)
23. Run:
    ```
    cd ~/raspberry_pi && chmod +x install.sh && sudo ./install.sh
    ```

    | It asks | Type |
    |---|---|
    | Cart page URL | `https://medicart-iub.vercel.app/cart` |
    | Admin password for the Wi-Fi setup page | a new password |
    | Password of the setup hotspot | another new password |
    | Path to medicart.py | `/home/medicart/MideCart/vision/medicart.py` |
    | Set up driving control? | `y` |
    | Cart account email | `cart01@medicart.iub` |
    | Cart account password | the cart password from Firebase |
    | Firebase apiKey / databaseURL | press **Enter** (both) |
    | Password for the Pi dashboard from other devices | a new password (write it down) |
    | Python for medicart.py | `/home/medicart/MideCart/venv/bin/python` |

24. Auto-login, screen always on, and camera/mic for Chromium (Ubuntu). Then reboot:
    ```
    sudo sed -i 's/^#\?\s*AutomaticLoginEnable.*/AutomaticLoginEnable=True/; s/^#\?\s*AutomaticLogin\s*=.*/AutomaticLogin=medicart/; s/^#\?\s*WaylandEnable.*/WaylandEnable=false/' /etc/gdm3/custom.conf
    gsettings set org.gnome.desktop.session idle-delay 0
    gsettings set org.gnome.desktop.screensaver lock-enabled false
    sudo snap connect chromium:camera
    sudo snap connect chromium:audio-record
    sudo reboot
    ```
25. ✅ The cart page opens full screen. Sign in with `cart01@medicart.iub`. Hold the gear for 2 s → choose the mic and speaker → **Test speaker**.

## F. The three ways to drive
26. **Pi dashboard** (live camera): on a laptop on the same Wi-Fi, open **http://medicart.local:5000**. Username: anything; password: the dashboard password from step 23.
27. **Vercel console:** open `medicart-iub.vercel.app` → **Drive**.
28. **SSH, no internet needed:** `ssh medicart@medicart.local`, then `medicart-drive`. Keys: 2 = Manual, 1 = Auto, 0 = stop, hold W A S D.
29. After every restart the robot is **STOPPED**. Choose **Auto** or **Manual** on any of the three.

## G. Website update (once)
30. GitHub → your repo → **Add file → Upload files** → `index.html`, `cart.html`, `vercel.json` from `medicart-system` → **Commit**. Wait 1 minute, then open the console and press **Ctrl + F5**.

## Problems
| You see | Do |
|---|---|
| Dashboard page doesn't open | `sudo systemctl status medicart-vision`, then `journalctl -u medicart-vision -n 40` |
| `Dashboard off (No module named 'flask')` | `~/MideCart/venv/bin/pip install flask requests` |
| Console Drive page: **offline** | `journalctl -u medicart-vision \| grep FIREBASE`. *sign-in failed* → run `sudo ./install.sh` again with the right cart password |
| Manual greyed out ("needs firmware 3.1") | upload `arduino/medicart/medicart.ino` again (step 3) |
| Robot won't move in Manual | the camera says STOP (person, or vision not running): the dashboard shows why |
| Black screen after the boot logo | HDMI port next to the power socket; official power supply |
