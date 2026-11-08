"""Stand-in for the Screen+ engine's side of the bus, to test a runtime build before the engine has it.

    python -I fake_engine.py <runtime-exe> <project.pck> [--grant audio,camera]

Creates the pipe (Windows) or socket (elsewhere), starts the runtime with --sp-pipe, sends sp.hello, a fake
spectrum at 30 Hz, pauses and resumes once, changes a property, prints everything the pack sends, then sp.stop.
Every call the pack makes is answered with an echo of its params.
"""

import json
import math
import os
import socket
import struct
import subprocess
import sys
import threading
import time

NAME = f"ScreenPlus.spx.test.{os.getpid()}"


class WinPipe:
    def __init__(self, name):
        import ctypes
        from ctypes import wintypes

        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.CreateNamedPipeW.restype = wintypes.HANDLE
        self.ct, self.wt = ctypes, wintypes
        # Duplex, byte mode, one instance, overlapped so reads and writes can run on two threads.
        self.h = self.k.CreateNamedPipeW("\\\\.\\pipe\\" + name, 3 | 0x40000000, 0, 1, 1 << 16, 1 << 16, 0, None)
        if self.h in (None, wintypes.HANDLE(-1).value):
            raise OSError(ctypes.get_last_error())

    def _io(self, write, buf):
        ct, wt = self.ct, self.wt

        class OVERLAPPED(ct.Structure):
            _fields_ = [("i", ct.c_void_p), ("ih", ct.c_void_p), ("o", wt.DWORD), ("oh", wt.DWORD), ("e", wt.HANDLE)]

        ov = OVERLAPPED()
        ov.e = self.k.CreateEventW(None, True, False, None)
        n = wt.DWORD()
        f = self.k.WriteFile if write else self.k.ReadFile
        ok = f(self.h, buf, len(buf), None, ct.byref(ov))
        if not ok and ct.get_last_error() != 997:  # ERROR_IO_PENDING
            self.k.CloseHandle(ov.e)
            raise EOFError
        ok = self.k.GetOverlappedResult(self.h, ct.byref(ov), ct.byref(n), True)
        self.k.CloseHandle(ov.e)
        if not ok or n.value == 0:
            raise EOFError
        return n.value

    def accept(self):
        self.k.ConnectNamedPipe(self.h, None)

    def recv(self, size):
        buf = self.ct.create_string_buffer(size)
        n = self._io(False, buf)
        return buf.raw[:n]

    def send(self, data):
        while data:
            n = self._io(True, self.ct.create_string_buffer(data, len(data)))
            data = data[n:]


class UnixPipe:
    def __init__(self, name):
        path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), name + ".sock")
        self.srv = socket.socket(socket.AF_UNIX)
        self.srv.bind(path)
        self.srv.listen(1)
        self.path = path

    def accept(self):
        self.c, _ = self.srv.accept()
        os.unlink(self.path)

    def recv(self, size):
        d = self.c.recv(size)
        if not d:
            raise EOFError
        return d

    def send(self, data):
        self.c.sendall(data)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    grants = {}
    if "--grant" in sys.argv:
        grants = {g: True for g in sys.argv[sys.argv.index("--grant") + 1].split(",")}
    pipe = WinPipe(NAME) if os.name == "nt" else UnixPipe(NAME)
    proc = subprocess.Popen([sys.argv[1], "--main-pack", sys.argv[2], "--", "--sp-pipe", NAME])
    pipe.accept()
    lock = threading.Lock()

    def send(method, params=None, id=None, result=None):
        msg = {"jsonrpc": "2.0"}
        if method:
            msg["method"] = method
            if params is not None:
                msg["params"] = params
        else:
            msg["id"], msg["result"] = id, result
        data = json.dumps(msg).encode()
        with lock:
            pipe.send(struct.pack("<I", len(data)) + data)

    def reader():
        buf = b""
        try:
            while True:
                while len(buf) < 4:
                    buf += pipe.recv(1 << 16)
                (n,) = struct.unpack("<I", buf[:4])
                while len(buf) < 4 + n:
                    buf += pipe.recv(1 << 16)
                msg, buf = json.loads(buf[4 : 4 + n]), buf[4 + n :]
                print("pack:", msg, flush=True)
                if "id" in msg:
                    send(None, id=msg["id"], result={"echo": msg.get("params")})
        except EOFError:
            print("pack closed the pipe", flush=True)

    threading.Thread(target=reader, daemon=True).start()
    send("sp.hello", {"version": "fake-0", "display": {"w": 1920, "h": 1080, "hz": 60, "scale": 1, "index": 0},
                      "props": {"speed": 1}, "granted": grants})
    t0 = time.time()
    script = {3.0: ("sp.pause", {"reason": "test"}), 5.0: ("sp.resume", {}), 6.0: ("sp.props", {"speed": 2}),
              9.0: ("sp.stop", None)}
    while proc.poll() is None and time.time() - t0 < 12:
        t = time.time() - t0
        for at in [a for a in script if a <= t]:
            m, p = script.pop(at)
            print("engine:", m, flush=True)
            send(m, p)
        spec = [abs(math.sin(t * 3 + i * 0.3)) for i in range(64)]
        try:
            send("audio.frame", {"spectrum": spec, "level": sum(spec) / 64})
        except (EOFError, OSError):
            break
        time.sleep(1 / 30)
    print("runtime exit code:", proc.wait(timeout=5))


if __name__ == "__main__":
    main()
