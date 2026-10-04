"""Host side of silicon's COM2 bridge: tells the guest how big the QEMU window is (so the OS
follows the window size) and keeps the clipboards of Windows and silicon in sync.

Protocol (text lines): see src/kernel/drv/bridge.cpp.
"""
import base64
import ctypes
import socket
import threading
import time

IS_WIN = hasattr(ctypes, "windll")


# ---------------------------------------------------------------- Windows helpers
def find_window(pid):
    if not IS_WIN:
        return 0
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    enum = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(enum(cb), 0)
    return found[0] if found else 0


def client_size(hwnd):
    from ctypes import wintypes
    rc = wintypes.RECT()
    if not ctypes.windll.user32.GetClientRect(hwnd, ctypes.byref(rc)):
        return None
    return rc.right - rc.left, rc.bottom - rc.top


def _clip_api():
    from ctypes import wintypes
    u = ctypes.windll.user32
    k = ctypes.windll.kernel32
    u.GetClipboardData.restype = wintypes.HANDLE
    u.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
    k.GlobalAlloc.restype = wintypes.HANDLE
    k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [wintypes.HANDLE]
    k.GlobalUnlock.argtypes = [wintypes.HANDLE]
    return u, k


def clipboard_get():
    if not IS_WIN:
        return None
    u, k = _clip_api()
    for _ in range(5):
        if u.OpenClipboard(None):
            break
        time.sleep(0.01)
    else:
        return None
    try:
        h = u.GetClipboardData(13)          # CF_UNICODETEXT
        if not h:
            return None
        p = k.GlobalLock(h)
        if not p:
            return None
        try:
            return ctypes.wstring_at(p)
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


def clipboard_set(text):
    if not IS_WIN:
        return
    u, k = _clip_api()
    data = (text + "\0").encode("utf-16-le")
    for _ in range(5):
        if u.OpenClipboard(None):
            break
        time.sleep(0.01)
    else:
        return
    try:
        u.EmptyClipboard()
        h = k.GlobalAlloc(0x0002, len(data))     # GMEM_MOVEABLE
        p = k.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k.GlobalUnlock(h)
        u.SetClipboardData(13, h)
    finally:
        u.CloseClipboard()


# ---------------------------------------------------------------- the bridge
class Bridge:
    def __init__(self, port, pid, hz=60):
        self.port = port
        self.pid = pid
        self.hz = hz
        self.sock = None
        self.alive = True
        self.last_size = None
        self.last_clip = None           # last text we know both sides have
        self.lock = threading.Lock()
        self.hello = threading.Event()

    def start(self):
        threading.Thread(target=self.run, daemon=True).start()

    def send(self, line):
        with self.lock:
            try:
                self.sock.sendall((line + "\n").encode("ascii"))
            except Exception:
                pass

    def connect(self):
        while self.alive:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                s.settimeout(0.2)
                return s
            except OSError:
                time.sleep(0.2)
        return None

    def push_state(self):
        self.send("H %d" % self.hz)
        self.last_size = None            # force an R line
        self.push_size()
        t = clipboard_get()
        if t:
            self.last_clip = t
            self.send("C " + base64.b64encode(t.encode("utf-8")).decode("ascii"))

    def push_size(self):
        hwnd = find_window(self.pid)
        if not hwnd:
            return
        sz = client_size(hwnd)
        if not sz or sz[0] < 200 or sz[1] < 150 or sz == self.last_size:
            return
        self.last_size = sz
        self.send("R %d %d" % sz)

    def handle(self, line):
        if line == "HI":
            self.hello.set()
            self.push_state()
        elif line.startswith("C "):
            try:
                text = base64.b64decode(line[2:]).decode("utf-8", "replace")
            except Exception:
                return
            self.last_clip = text
            clipboard_set(text)

    def run(self):
        self.sock = self.connect()
        if not self.sock:
            return
        buf = b""
        last_tick = 0
        last_seq = None
        while self.alive:
            try:
                data = self.sock.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.handle(line.decode("ascii", "ignore").strip())
            except socket.timeout:
                pass
            except OSError:
                break
            now = time.time()
            if now - last_tick > 0.08 and IS_WIN:
                last_tick = now
                self.push_size()
                seq = ctypes.windll.user32.GetClipboardSequenceNumber()
                if seq != last_seq:
                    last_seq = seq
                    t = clipboard_get()
                    if t and t != self.last_clip and self.hello.is_set():
                        self.last_clip = t
                        self.send("C " + base64.b64encode(t.encode("utf-8")).decode("ascii"))

    def stop(self):
        self.alive = False
