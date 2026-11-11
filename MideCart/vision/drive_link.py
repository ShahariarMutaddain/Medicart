"""MediCart 3.0 — turns the dashboard's decision into Arduino commands (Rifat's protocol).

    requested_drive() -> "STOP"            send STOP
                      -> "MODE:AUTO"       send MODE:AUTO   (= path clear, line following)
                      -> "MODE:MANUAL",l,r send MODE:MANUAL then DRIVE:l,r

The Arduino stops by itself 750 ms after the last command, so:
  * a change is sent at once
  * AUTO / STOP are repeated every HEARTBEAT_SECONDS (0.35 s, as before)
  * MANUAL is repeated every MANUAL_SECONDS (0.1 s) so driving feels immediate
Also parses what the Arduino prints (firmware 3.0: FW / STAT / DBG motor lines).
"""
from __future__ import annotations

import re
from typing import Callable, Optional

HEARTBEAT_SECONDS = 0.35
MANUAL_SECONDS = 0.10
STAT_SECONDS = 1.0


class DriveLink:
    def __init__(self, send: Callable[[str], bool]) -> None:
        self.send = send
        self.last_cmd: Optional[str] = None
        self.last_lr: Optional[tuple[int, int]] = None
        self.last_sent = -1e9
        self.last_stat = -1e9
        self.quiet_sent = False

    def reset(self) -> None:
        """Call after the Arduino (re)connects."""
        self.last_cmd = None
        self.last_lr = None
        self.last_sent = -1e9
        self.quiet_sent = False

    def step(self, now: float, drive_cmd: str, left: int = 0, right: int = 0) -> bool:
        """Send what is due. Returns False if the serial port failed."""
        if not self.quiet_sent:
            # firmware 3.0 prints a live view for the Serial Monitor; the Pi doesn't need it
            if not self.send("VIEW 0"):
                return False
            self.quiet_sent = True
        manual = drive_cmd == "MODE:MANUAL"
        changed = drive_cmd != self.last_cmd or (manual and (left, right) != self.last_lr)
        interval = MANUAL_SECONDS if manual else HEARTBEAT_SECONDS
        if changed or now - self.last_sent >= interval:
            if not self.send(drive_cmd):
                return False
            if manual and not self.send(f"DRIVE:{int(left)},{int(right)}"):
                return False
            self.last_cmd = drive_cmd
            self.last_lr = (left, right) if manual else None
            self.last_sent = now
        if now - self.last_stat >= STAT_SECONDS:
            self.last_stat = now
            if not self.send("STAT"):
                return False
        return True


_STAT = re.compile(r"(\w+)=([^\s]+)")


def parse_arduino_line(line: str) -> tuple[dict, bool]:
    """Returns (info, show_in_log). info keys: firmware, line, line_lost, motor."""
    info: dict = {}
    if line.startswith("FW "):
        info["firmware"] = line[3:].strip()
        return info, True
    if line.startswith("STAT "):
        kv = dict(_STAT.findall(line))
        if "line" in kv:
            info["line"] = kv["line"]
        if "lost" in kv:
            info["line_lost"] = kv["lost"] == "1"
        if "motor" in kv and "," in kv["motor"]:
            try:
                a, b = kv["motor"].split(",", 1)
                info["motor"] = (int(a), int(b))
            except ValueError:
                pass
        if "fw" in kv:
            info["firmware"] = f"medicart {kv['fw']}"
        return info, False                      # once a second: keep it out of the log
    if line.startswith("DBG motor="):
        try:
            a, b = line[len("DBG motor="):].split(",", 1)
            info["motor"] = (int(a), int(b))
        except ValueError:
            pass
        return info, True
    if line.startswith("VIEW "):
        return info, False
    return info, True
