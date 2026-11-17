"""Nest PC Server - watch the videos on your PC from Nest on your Vita, over Wi-Fi.

1. Double-click "Nest PC Server.bat".
2. Choose the folder with your shows/films (it's remembered) and press Start.
3. On the Vita: Nest > Settings > Library > Watch from your PC (Wi-Fi),
   and type the address this window shows (for example 192.168.1.20).

Only the folder you choose is shared, read-only, and only on your home network.
Windows may ask whether Python can use the network: allow it on Private networks.
The PC needs to stay on (and awake) while you watch.
Videos still need to be MP4 (H.264 + AAC) for the Vita to play them.
"""
import json
import os
import socket
import sys
import threading
import tkinter as tk
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from tkinter import filedialog, messagebox, ttk

APP = "Nest PC Server"
PORT = 8642
CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nest_pc_server.json")
FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM = 0x2, 0x4


def lan_addresses():
    ips = []
    try:  # the address Windows would use to reach the network (no data is sent)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except OSError:
        pass
    return ips or ["127.0.0.1"]


class Handler(BaseHTTPRequestHandler):
    root = ""
    log_cb = None
    protocol_version = "HTTP/1.1"  # keep-alive: the Vita reuses one connection while streaming

    def log_message(self, fmt, *args):
        pass

    def resolve(self):
        u = urllib.parse.urlsplit(self.path)
        if not u.path.startswith("/f"):
            return None, u.query
        rel = urllib.parse.unquote(u.path[2:]).lstrip("/")
        full = os.path.normpath(os.path.join(self.root, rel))
        base = os.path.normpath(self.root)
        if full != base and not full.startswith(base + os.sep):
            return None, u.query  # never outside the shared folder
        return full, u.query

    def send_text(self, code, text):
        b = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(b)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        full, query = self.resolve()
        if not full or not os.path.exists(full):
            return self.send_text(404, "not found")
        if os.path.isdir(full):
            lines = ["NEST-LIST"]
            try:
                with os.scandir(full) as it:
                    for de in it:
                        try:
                            st = de.stat()
                        except OSError:
                            continue
                        attrs = getattr(st, "st_file_attributes", 0)
                        if attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) or de.name.startswith("."):
                            continue
                        if "\t" in de.name or "\n" in de.name:
                            continue
                        isdir = de.is_dir()
                        lines.append(f"{'d' if isdir else 'f'}\t{0 if isdir else st.st_size}\t{de.name}")
            except OSError as e:
                return self.send_text(500, str(e))
            return self.send_text(200, "\n".join(lines) + "\n")
        size = os.path.getsize(full)
        if query == "size":
            return self.send_text(200, str(size))
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        partial = False
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].split(",")[0].partition("-")
            try:
                if a:
                    start = int(a)
                    end = int(b) if b else size - 1
                else:
                    start = max(0, size - int(b))
                end = min(end, size - 1)
                partial = True
            except ValueError:
                pass
            if start >= size or start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        length = end - start + 1
        self.send_response(206 if partial else 200)
        ext = os.path.splitext(full)[1].lower()
        ctype = {".mp4": "video/mp4", ".m4v": "video/mp4", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
                 ".png": "image/png", ".json": "application/json", ".srt": "text/plain"}.get(ext, "application/octet-stream")
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if self.command == "HEAD":
            return
        if self.log_cb and ext in (".mp4", ".m4v") and start == 0:
            self.log_cb(f"Vita {self.client_address[0]} is playing {os.path.basename(full)}")
        try:
            with open(full, "rb") as f:
                f.seek(start)
                left = length
                while left > 0:
                    chunk = f.read(min(256 * 1024, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
        except (ConnectionError, OSError):
            pass


class App:
    def __init__(self, root):
        self.root = root
        root.title(APP)
        root.geometry("620x420")
        self.server = None
        self.folder = tk.StringVar(value=self.load().get("folder", ""))
        self.addr = tk.StringVar(value="Press Start")
        self.state = tk.StringVar(value="Stopped")

        f = ttk.Frame(root, padding=16)
        f.pack(fill="both", expand=True)
        ttk.Label(f, text="Folder to share with your Vita:").pack(anchor="w")
        row = ttk.Frame(f)
        row.pack(fill="x", pady=(4, 12))
        ttk.Entry(row, textvariable=self.folder).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Browse…", command=self.browse).pack(side="left", padx=6)
        btns = ttk.Frame(f)
        btns.pack(fill="x")
        self.start_b = ttk.Button(btns, text="Start", command=self.start)
        self.start_b.pack(side="left")
        ttk.Button(btns, text="Stop", command=self.stop).pack(side="left", padx=6)
        ttk.Label(btns, textvariable=self.state).pack(side="left", padx=12)
        ttk.Label(f, text="Type this into Nest on your Vita:", padding=(0, 18, 0, 2)).pack(anchor="w")
        tk.Label(f, textvariable=self.addr, font=("Segoe UI", 26, "bold")).pack(anchor="w")
        ttk.Label(f, text="Nest › Settings › Library › Watch from your PC (Wi-Fi)",
                  foreground="#666").pack(anchor="w")
        self.log = tk.Listbox(f, height=6)
        self.log.pack(fill="both", expand=True, pady=(14, 0))
        root.protocol("WM_DELETE_WINDOW", self.quit)
        if self.folder.get() and os.path.isdir(self.folder.get()):
            root.after(300, self.start)

    def load(self):
        try:
            with open(CONF, encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {}

    def save(self):
        try:
            with open(CONF, "w", encoding="utf-8") as fh:
                json.dump({"folder": self.folder.get()}, fh)
        except OSError:
            pass

    def browse(self):
        d = filedialog.askdirectory(title="Choose the folder with your videos")
        if d:
            self.folder.set(os.path.normpath(d))
            if self.server:
                self.stop()
                self.start()

    def note(self, msg):
        self.root.after(0, lambda: (self.log.insert(0, msg), self.log.delete(60, "end")))

    def start(self):
        d = self.folder.get().strip()
        if not os.path.isdir(d):
            messagebox.showwarning(APP, "Choose the folder with your videos first.")
            return
        if self.server:
            return
        Handler.root = d
        Handler.log_cb = self.note
        try:
            self.server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
        except OSError as e:
            messagebox.showerror(APP, f"Couldn't start on port {PORT}: {e}\n\nIs Nest PC Server already open?")
            return
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        ips = lan_addresses()
        self.addr.set(ips[0])
        self.state.set(f"Sharing {os.path.basename(d) or d} on port {PORT}")
        self.note("Started. Other addresses for this PC: " + ", ".join(ips[1:]) if len(ips) > 1 else "Started.")
        self.save()

    def stop(self):
        if self.server:
            s = self.server
            self.server = None
            threading.Thread(target=lambda: (s.shutdown(), s.server_close()), daemon=True).start()
        self.state.set("Stopped")
        self.addr.set("Press Start")

    def quit(self):
        self.stop()
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
