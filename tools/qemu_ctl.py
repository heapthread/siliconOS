"""Drive a headless silicon QEMU through its monitor (tcp:127.0.0.1:4444).

    python tools/qemu_ctl.py start                 boot headless (kills an old one)
    python tools/qemu_ctl.py type "ls -l\\n"        type text (\\n = Enter)
    python tools/qemu_ctl.py key ctrl-c            send one key combo
    python tools/qemu_ctl.py shot out.png          screenshot to PNG
    python tools/qemu_ctl.py mouse X Y [btn]       move to absolute X Y, optional click (1=left 2=right)
    python tools/qemu_ctl.py drag X1 Y1 X2 Y2      left-drag between two points
    python tools/qemu_ctl.py wait 2                sleep seconds
    python tools/qemu_ctl.py quit
Several actions can be chained: start wait 3 type "help\\n" shot a.png
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

KEYS = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent", "\\": "backslash",
    ",": "comma", ".": "dot", "/": "slash",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right", ":": "semicolon",
    '"': "apostrophe", "~": "grave_accent", "|": "backslash", "<": "comma", ">": "dot", "?": "slash",
}


class Monitor:
    def __init__(self):
        self.s = None
        for _ in range(50):
            try:
                self.s = socket.create_connection(("127.0.0.1", 4444), timeout=5)
                break
            except OSError:
                time.sleep(0.2)
        if not self.s:
            sys.exit("qemu_ctl: monitor not reachable")
        self.read()

    def read(self):
        data = b""
        self.s.settimeout(0.5)
        try:
            while not data.endswith(b"(qemu) "):
                chunk = self.s.recv(65536)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data.decode(errors="replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        return self.read()


def keyname(ch):
    if ch in SHIFTED:
        return "shift-" + SHIFTED[ch]
    if ch in KEYS:
        return KEYS[ch]
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch


def main():
    args = sys.argv[1:]
    mon = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "start":
            subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"], capture_output=True)
            time.sleep(0.5)
            import build
            subprocess.Popen(build.qemu_cmd(headless=True), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)
            mon = Monitor()
        elif a == "wait":
            time.sleep(float(args[i + 1])); i += 1
        else:
            mon = mon or Monitor()
            if a == "type":
                text = args[i + 1].encode().decode("unicode_escape"); i += 1
                for ch in text:
                    mon.cmd("sendkey " + keyname(ch))
                    time.sleep(0.03)
            elif a == "key":
                mon.cmd("sendkey " + args[i + 1]); i += 1
            elif a == "shot":
                out = os.path.abspath(args[i + 1]); i += 1
                os.makedirs(os.path.dirname(out), exist_ok=True)
                ppm = out + ".ppm"
                mon.cmd("screendump " + ppm.replace("\\", "/"))
                time.sleep(0.5)
                from PIL import Image
                Image.open(ppm).save(out)
                os.remove(ppm)
                print("saved", out)
            elif a == "mouse":
                x, y = int(args[i + 1]), int(args[i + 2]); i += 2
                # relative moves in small steps (QEMU's PS/2 queue holds only a few packets):
                # slam into the top-left corner, then walk to the target
                for _ in range(12):
                    mon.cmd("mouse_move -100 -100")
                    time.sleep(0.015)
                tx, ty = x, y
                while tx or ty:
                    sx, sy = max(-100, min(100, tx)), max(-100, min(100, ty))
                    mon.cmd(f"mouse_move {sx} {sy}")
                    tx -= sx
                    ty -= sy
                    time.sleep(0.015)
                time.sleep(0.05)
                if i + 1 < len(args) and args[i + 1] in ("1", "2"):
                    b = int(args[i + 1]); i += 1
                    mon.cmd(f"mouse_button {b}")
                    time.sleep(0.08)
                    mon.cmd("mouse_button 0")
            elif a == "drag":
                x1, y1, x2, y2 = (int(v) for v in args[i + 1:i + 5]); i += 4
                for _ in range(12):
                    mon.cmd("mouse_move -100 -100")
                    time.sleep(0.015)
                for tx, ty in ((x1, y1),):
                    while tx or ty:
                        sx, sy = max(-100, min(100, tx)), max(-100, min(100, ty))
                        mon.cmd(f"mouse_move {sx} {sy}")
                        tx -= sx; ty -= sy
                        time.sleep(0.015)
                mon.cmd("mouse_button 1")
                time.sleep(0.1)
                dx, dy = x2 - x1, y2 - y1
                steps = max(1, max(abs(dx), abs(dy)) // 20)
                for k in range(steps):
                    mon.cmd(f"mouse_move {dx // steps} {dy // steps}")
                    time.sleep(0.03)
                time.sleep(0.1)
                mon.cmd("mouse_button 0")
                time.sleep(0.1)
            elif a == "cmd":
                print(mon.cmd(args[i + 1])); i += 1
            elif a == "quit":
                try:
                    mon.cmd("quit")
                except OSError:
                    pass
            else:
                sys.exit("qemu_ctl: unknown action " + a)
        i += 1


if __name__ == "__main__":
    main()
