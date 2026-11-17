"""Nest Finder - find files and folders you can't see, or think are gone.

Point it at a folder or a whole drive (an SD card, a USB stick...).

Quick scan (seconds to minutes) finds everything Windows Explorer hides or chokes on:
  hidden and system files, names ending in a space or dot, paths too long for
  Explorer, chkdsk leftovers (FOUND.000, *.CHK), Recycle Bin contents, empty files.

Deep scan (exFAT drives such as Vita / Switch / camera cards; needs administrator)
reads the raw card sector by sector and finds every file record still on it,
including DELETED files and files that fell out of their folder after the card
was unplugged too early. Found files can be recovered (copied to another drive).

Then search, open, unhide, move, delete, or recover what it found.
Nothing is changed on the drive unless you press a button and confirm.
"""
import ctypes
import os
import queue
import re
import shutil
import stat
import struct
import sys
import threading
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import filedialog, messagebox, ttk

APP = "Nest Finder"
VIDEO = {".mp4", ".m4v", ".mkv", ".avi", ".mov", ".wmv", ".ts", ".webm", ".flv", ".mpg", ".mpeg"}
BOOK = {".cbz", ".cbr", ".pdf", ".epub", ".zip", ".rar", ".7z"}
PIC = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"}
AUDIO = {".mp3", ".flac", ".m4a", ".ogg", ".wav", ".aac"}
GAME = {".vpk", ".iso", ".cso", ".pbp", ".bin", ".sfo", ".self", ".suprx", ".skprx"}

FILE_ATTRIBUTE_HIDDEN = 0x2
FILE_ATTRIBUTE_SYSTEM = 0x4


def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def long_path(p):
    """Windows path that also works for very long names and names ending in space/dot."""
    if p.startswith("\\\\?\\"):
        return p
    # abspath would trim a trailing space/dot - exactly the names we must keep
    p = p.replace("/", "\\") if os.path.isabs(p) else os.path.abspath(p)
    if p.startswith("\\\\?\\"):
        return p
    if p.startswith("\\\\"):
        return "\\\\?\\UNC\\" + p[2:]
    return "\\\\?\\" + p


def short(p):
    return p[4:] if p.startswith("\\\\?\\") else p


def human(n):
    if n is None:
        return ""
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


def kind_of(name):
    ext = os.path.splitext(name)[1].lower()
    if ext in VIDEO:
        return "video"
    if ext in BOOK:
        return "book"
    if ext in PIC:
        return "picture"
    if ext in AUDIO:
        return "music"
    if ext in GAME:
        return "game/app"
    if ext == ".chk":
        return "chkdsk piece"
    return ""


def recycle(path):
    """Send to the Recycle Bin when the drive has one. Returns True on success."""

    class SHFILEOPSTRUCTW(ctypes.Structure):
        _fields_ = [("hwnd", wintypes.HWND), ("wFunc", wintypes.UINT), ("pFrom", wintypes.LPCWSTR),
                    ("pTo", wintypes.LPCWSTR), ("fFlags", ctypes.c_ushort), ("fAnyOperationsAborted", wintypes.BOOL),
                    ("hNameMappings", ctypes.c_void_p), ("lpszProgressTitle", wintypes.LPCWSTR)]

    FO_DELETE, FOF_ALLOWUNDO, FOF_NOCONFIRMATION, FOF_SILENT, FOF_NOERRORUI = 3, 0x40, 0x10, 0x4, 0x400
    op = SHFILEOPSTRUCTW(None, FO_DELETE, short(path) + "\0", None,
                         FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI, False, None, None)
    return ctypes.windll.shell32.SHFileOperationW(ctypes.byref(op)) == 0 and not os.path.lexists(long_path(path))


def has_recycle_bin(path):
    drive = os.path.splitdrive(os.path.abspath(path))[0]
    try:
        DRIVE_FIXED = 3
        return ctypes.windll.kernel32.GetDriveTypeW(drive + "\\") == DRIVE_FIXED
    except Exception:
        return False


# --------------------------------------------------------------------------- quick scan
class Item:
    __slots__ = ("path", "name", "size", "is_dir", "why", "kind", "deep")

    def __init__(self, path, name, size, is_dir, why, kind="", deep=None):
        self.path, self.name, self.size, self.is_dir = path, name, size, is_dir
        self.why, self.kind, self.deep = why, kind, deep


def quick_scan(root, out, stop, progress):
    root = long_path(root)
    seen = 0
    stack = [(root, 0)]
    while stack and not stop.is_set():
        d, depth = stack.pop()
        try:
            it = list(os.scandir(d))
        except OSError as e:
            out.put(Item(short(d), os.path.basename(d), None, True, f"can't be opened ({e.strerror})"))
            continue
        if not it and d != root:
            out.put(Item(short(d), os.path.basename(d), 0, True, "empty folder"))
        for de in it:
            seen += 1
            if seen % 500 == 0:
                progress(seen, short(de.path))
            name = de.name
            try:
                st = de.stat(follow_symlinks=False)
            except OSError:
                out.put(Item(short(de.path), name, None, False, "can't be read"))
                continue
            attrs = getattr(st, "st_file_attributes", 0)
            isdir = stat.S_ISDIR(st.st_mode)
            why = []
            if attrs & FILE_ATTRIBUTE_HIDDEN:
                why.append("hidden")
            if attrs & FILE_ATTRIBUTE_SYSTEM:
                why.append("system")
            if name != name.rstrip(" .") or name != name.lstrip(" "):
                why.append("name ends in space/dot (Explorer can't open it)")
            if len(short(de.path)) >= 255:
                why.append("path too long for Explorer")
            if re.fullmatch(r"FOUND\.\d{3}", name, re.I):
                why.append("chkdsk recovered folder")
            if name.lower().endswith(".chk"):
                why.append("chkdsk recovered piece")
            if name.lower() in ("$recycle.bin", "recycler", ".trashes", ".trash-1000"):
                why.append("recycle bin")
            if "$recycle.bin" in de.path.lower() and not why:
                why.append("in the recycle bin")
            if not isdir and st.st_size == 0:
                why.append("empty file (0 bytes)")
            if any(ord(c) < 32 for c in name):
                why.append("strange characters in name")
            size = None if isdir else st.st_size
            out.put(Item(short(de.path), name, size, isdir, ", ".join(why), kind_of(name)))
            if isdir and depth < 64 and name.lower() != "system volume information":
                stack.append((de.path, depth + 1))
    progress(seen, "")


# --------------------------------------------------------------------------- deep scan (exFAT)
class ExFat:
    def __init__(self, letter):
        self.letter = letter.rstrip(":\\/")[0].upper()
        self.f = open(f"\\\\.\\{self.letter}:", "rb", buffering=0)
        boot = self.f.read(512)
        if boot[3:11] != b"EXFAT   ":
            self.f.close()
            raise ValueError("This drive isn't exFAT, so deep scan can't read it.")
        self.bps = 1 << boot[0x6C]
        self.cs = self.bps << boot[0x6D]
        self.fat_off = struct.unpack_from("<I", boot, 0x50)[0] * self.bps
        self.heap = struct.unpack_from("<I", boot, 0x58)[0] * self.bps
        self.nclusters = struct.unpack_from("<I", boot, 0x5C)[0]
        self.total = self.heap + self.nclusters * self.cs
        self.lock = threading.Lock()

    def read(self, off, n):
        """aligned raw read"""
        a = off - off % self.bps
        end = off + n
        b = end + (-end) % self.bps
        with self.lock:
            self.f.seek(a)
            data = self.f.read(b - a)
        return data[off - a: off - a + n]

    def fat(self, c):
        return struct.unpack("<I", self.read(self.fat_off + c * 4, 4))[0]

    def runs(self, first, length, nofat):
        """(offset, size) pieces of a file's data"""
        if first < 2 or length <= 0:
            return []
        if nofat:
            return [(self.heap + (first - 2) * self.cs, length)]
        out, c, left, guard = [], first, length, 0
        while left > 0 and 2 <= c < self.nclusters + 2 and guard < 50_000_000:
            n = min(self.cs, left)
            off = self.heap + (c - 2) * self.cs
            if out and out[-1][0] + out[-1][1] == off:
                out[-1] = (out[-1][0], out[-1][1] + n)
            else:
                out.append((off, n))
            left -= n
            c = self.fat(c)
            guard += 1
        return out

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


ENTRY_RE = re.compile(rb"(?=[\x85\x05].{31}[\xc0\x40])", re.S)  # lookahead: overlapping matches


def parse_set(buf, i):
    """Parse a directory entry set at buf[i]. Returns dict or None."""
    if i + 96 > len(buf):
        return None
    sec = buf[i + 1]
    if not 2 <= sec <= 18 or i + 32 * (sec + 1) > len(buf):
        return None
    raw = bytearray(buf[i:i + 32 * (sec + 1)])
    deleted = raw[0] == 0x05
    for k in range(sec + 1):  # undo the "deleted" bit so the checksum matches
        raw[32 * k] |= 0x80
    if raw[32] != 0xC0:
        return None
    for k in range(2, sec + 1):
        if raw[32 * k] not in (0xC1, 0xE0, 0xE1) and raw[32 * k] < 0xC2:
            return None
    chk = 0
    for n, b in enumerate(raw):
        if n in (2, 3):
            continue
        chk = ((chk << 15) | (chk >> 1)) + b & 0xFFFF
    if chk != struct.unpack_from("<H", raw, 2)[0]:
        return None
    attrs = struct.unpack_from("<H", raw, 4)[0]
    flags = raw[33]
    namelen = raw[35]
    valid = struct.unpack_from("<Q", raw, 40)[0]
    first = struct.unpack_from("<I", raw, 52)[0]
    length = struct.unpack_from("<Q", raw, 56)[0]
    name = b""
    for k in range(2, sec + 1):
        if raw[32 * k] == 0xC1:
            name += bytes(raw[32 * k + 2:32 * k + 32])
    try:
        name = name.decode("utf-16-le")[:namelen]
    except UnicodeDecodeError:
        return None
    if not name or namelen == 0 or any(c in name for c in '\\/:*?"<>|\0'):
        return None
    return {"name": name, "dir": bool(attrs & 0x10), "deleted": deleted, "first": first,
            "length": length, "valid": min(valid, length), "nofat": bool(flags & 2), "hidden": bool(attrs & 2)}


def deep_scan(letter, out, stop, progress, visible_names):
    fs = ExFat(letter)
    try:
        chunk = 8 * 1024 * 1024
        pos = fs.heap
        seen = set()
        t0 = time.time()
        while pos < fs.total and not stop.is_set():
            n = min(chunk + 32 * 19, fs.total - pos)
            buf = fs.read(pos, n)
            for m in ENTRY_RE.finditer(buf, 0, min(chunk, len(buf))):
                i = m.start()
                if (pos + i) % 32:
                    continue
                e = parse_set(buf, i)
                if not e:
                    continue
                key = (e["name"], e["first"], e["length"], e["dir"])
                if key in seen:
                    continue
                seen.add(key)
                visible = (e["name"].lower(), None if e["dir"] else e["length"]) in visible_names
                if e["deleted"]:
                    why = "DELETED - may be recoverable"
                elif not visible:
                    why = "on the card but not in any folder (lost)"
                else:
                    why = "normal (also visible)"
                if e["dir"]:
                    why += ", folder record"
                out.put(Item(f"{fs.letter}:\\? (raw offset {pos + i:,})", e["name"],
                             None if e["dir"] else e["length"], e["dir"], why, kind_of(e["name"]),
                             deep=(fs, e)))
            pos += chunk
            el = time.time() - t0
            done = pos - fs.heap
            rate = done / el if el > 0 else 0
            left = (fs.total - pos) / rate if rate else 0
            progress(done, f"{human(done)} of {human(fs.total - fs.heap)} read   "
                           f"{rate / 1e6:.1f} MB/s   about {int(left // 60)} min left",
                     frac=done / max(1, fs.total - fs.heap))
    finally:
        progress(0, "", frac=1.0)


def recover(item, dest_dir):
    fs, e = item.deep
    if e["dir"]:
        raise ValueError("That's a folder record; recover the files inside it instead.")
    target = os.path.join(dest_dir, e["name"])
    base, ext = os.path.splitext(target)
    k = 1
    while os.path.exists(target):
        target = f"{base} ({k}){ext}"
        k += 1
    left = e["valid"] or e["length"]
    with open(long_path(target), "wb") as o:
        for off, n in fs.runs(e["first"], e["length"], e["nofat"]):
            while n > 0 and left > 0:
                take = min(n, left, 4 * 1024 * 1024)
                o.write(fs.read(off, take))
                off += take
                n -= take
                left -= take
    return target


# --------------------------------------------------------------------------- UI
class App:
    def __init__(self, root, start=None, deep=False):
        self.root = root
        root.title(APP)
        root.geometry("1100x640")
        root.minsize(820, 420)
        self.items = []
        self.byid = {}
        self.q = queue.Queue()
        self.stop = threading.Event()
        self.worker = None
        self.folder = tk.StringVar(value=start or "")
        self.search = tk.StringVar()
        self.only_odd = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="Choose a folder or drive, then press Quick scan.")

        top = ttk.Frame(root, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="Look in:").pack(side="left")
        ttk.Entry(top, textvariable=self.folder, width=50).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(top, text="Browse…", command=self.browse).pack(side="left")
        ttk.Button(top, text="Quick scan", command=self.start_quick).pack(side="left", padx=(12, 4))
        ttk.Button(top, text="Deep scan (recover)", command=self.start_deep).pack(side="left", padx=4)
        ttk.Button(top, text="Stop", command=self.stop_scan).pack(side="left", padx=4)

        bar = ttk.Frame(root, padding=(8, 0))
        bar.pack(fill="x")
        ttk.Label(bar, text="Search:").pack(side="left")
        se = ttk.Entry(bar, textvariable=self.search, width=34)
        se.pack(side="left", padx=6)
        self.search.trace_add("write", lambda *a: self.refresh())
        ttk.Checkbutton(bar, text="Only show hidden / lost / odd things", variable=self.only_odd,
                        command=self.refresh).pack(side="left", padx=10)
        self.count = ttk.Label(bar, text="")
        self.count.pack(side="right")

        self.pb = ttk.Progressbar(root, mode="determinate", maximum=1000)
        self.pb.pack(fill="x", padx=8, pady=4)

        mid = ttk.Frame(root)
        mid.pack(fill="both", expand=True, padx=8)
        cols = ("name", "size", "type", "why", "where")
        self.tv = ttk.Treeview(mid, columns=cols, show="headings", selectmode="extended")
        for c, w, t in (("name", 300, "Name"), ("size", 80, "Size"), ("type", 80, "Type"),
                        ("why", 280, "Why it's listed"), ("where", 380, "Where")):
            self.tv.heading(c, text=t, command=lambda c=c: self.sort(c))
            self.tv.column(c, width=w, anchor="e" if c == "size" else "w")
        ys = ttk.Scrollbar(mid, orient="vertical", command=self.tv.yview)
        self.tv.configure(yscrollcommand=ys.set)
        self.tv.pack(side="left", fill="both", expand=True)
        ys.pack(side="right", fill="y")
        self.tv.bind("<Double-1>", lambda e: self.open_location())

        act = ttk.Frame(root, padding=8)
        act.pack(fill="x")
        for text, fn in (("Open location", self.open_location), ("Unhide (keep)", self.unhide),
                         ("Move to…", self.move), ("Delete…", self.delete),
                         ("Recover to…", self.recover)):
            ttk.Button(act, text=text, command=fn).pack(side="left", padx=4)
        ttk.Label(root, textvariable=self.status, padding=(8, 0, 8, 8)).pack(fill="x")

        self.sort_col, self.sort_rev = "where", False
        root.after(100, self.pump)
        if start and deep:
            root.after(300, self.start_deep)

    # ---- scanning
    def browse(self):
        d = filedialog.askdirectory(title="Choose a folder or drive to search")
        if d:
            self.folder.set(os.path.normpath(d))

    def busy(self):
        if self.worker and self.worker.is_alive():
            messagebox.showinfo(APP, "A scan is already running. Press Stop first.")
            return True
        return False

    def start_quick(self):
        root = self.folder.get().strip()
        if not root or not os.path.isdir(root):
            messagebox.showwarning(APP, "Choose a folder or drive first.")
            return
        if self.busy():
            return
        self.reset()
        self.status.set("Quick scan…")
        self.pb.configure(mode="indeterminate")
        self.pb.start(12)

        def run():
            quick_scan(root, self.q, self.stop, lambda n, p: self.q.put(("prog", f"{n:,} items checked   {p[-90:]}")))
            self.q.put(("done", "Quick scan finished."))

        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def start_deep(self):
        root = self.folder.get().strip()
        drive = os.path.splitdrive(os.path.abspath(root))[0] if root else ""
        if not drive:
            messagebox.showwarning(APP, "Choose the drive (or any folder on it) first.")
            return
        if self.busy():
            return
        if not is_admin():
            if messagebox.askyesno(APP, "Deep scan reads the raw card, which Windows only allows as administrator.\n\n"
                                        "Restart Nest Finder as administrator and start the deep scan?"):
                args = f'"{os.path.abspath(__file__)}" --deep "{drive}\\\\"'
                ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, args, None, 1)
                self.root.destroy()
            return
        if not messagebox.askokcancel(APP, f"Deep scan reads all of {drive} (only reads, never writes).\n\n"
                                           "On a big memory card this can take hours (about 1 hour per 60 GB on "
                                           "a USB 2 reader). You can Stop at any time and keep what was found."):
            return
        self.reset()
        self.status.set("Listing visible files first…")
        self.pb.configure(mode="determinate")

        def run():
            vis = set()
            for dp, dn, fn in os.walk(long_path(drive + "\\")):
                if self.stop.is_set():
                    break
                for n in dn:
                    vis.add((n.lower(), None))
                for n in fn:
                    try:
                        vis.add((n.lower(), os.path.getsize(os.path.join(dp, n))))
                    except OSError:
                        pass
            try:
                deep_scan(drive, self.q, self.stop,
                          lambda n, p, frac=None: self.q.put(("prog", p, frac)), vis)
                self.q.put(("done", "Deep scan finished. Select files and press Recover to…"))
            except PermissionError:
                self.q.put(("done", "Windows refused raw access. Run Nest Finder as administrator."))
            except Exception as e:
                self.q.put(("done", f"Deep scan stopped: {e}"))

        self.only_odd.set(True)
        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()

    def stop_scan(self):
        self.stop.set()

    def reset(self):
        self.stop = threading.Event()
        self.items = []
        self.tv.delete(*self.tv.get_children())
        self.pb["value"] = 0

    def pump(self):
        added = False
        try:
            for _ in range(4000):
                m = self.q.get_nowait()
                if isinstance(m, Item):
                    self.items.append(m)
                    added = True
                elif m[0] == "prog":
                    self.status.set(m[1])
                    if len(m) > 2 and m[2] is not None:
                        self.pb["value"] = int(m[2] * 1000)
                elif m[0] == "done":
                    self.pb.stop()
                    self.pb.configure(mode="determinate")
                    self.pb["value"] = 1000
                    self.status.set(m[1])
                    added = True
        except queue.Empty:
            pass
        if added:
            self.refresh()
        self.root.after(250, self.pump)

    # ---- list
    def visible(self):
        q = self.search.get().strip().lower()
        out = []
        for it in self.items:
            if self.only_odd.get() and (not it.why or it.why.startswith("normal")):
                continue
            if q and q not in it.name.lower() and q not in it.path.lower():
                continue
            out.append(it)
        key = {"name": lambda i: i.name.lower(), "size": lambda i: i.size or -1, "type": lambda i: i.kind,
               "why": lambda i: i.why, "where": lambda i: i.path.lower()}[self.sort_col]
        out.sort(key=key, reverse=self.sort_rev)
        return out

    def refresh(self):
        self.tv.delete(*self.tv.get_children())
        vis = self.visible()
        for idx, it in enumerate(vis[:20000]):
            where = os.path.dirname(it.path) if not it.deep else it.path
            typ = "folder" if it.is_dir else (it.kind or os.path.splitext(it.name)[1].lstrip(".").lower())
            self.tv.insert("", "end", iid=str(id(it)), values=(it.name, human(it.size), typ, it.why, where))
        self.byid = {str(id(it)): it for it in vis[:20000]}
        more = f" (showing first 20,000)" if len(vis) > 20000 else ""
        self.count.configure(text=f"{len(vis):,} shown of {len(self.items):,} found{more}")

    def sort(self, col):
        self.sort_rev = not self.sort_rev if self.sort_col == col else False
        self.sort_col = col
        self.refresh()

    def selected(self):
        return [self.byid[i] for i in self.tv.selection() if i in self.byid]

    # ---- actions
    def need(self, deep_ok=False, real_ok=True):
        sel = self.selected()
        if not sel:
            messagebox.showinfo(APP, "Select one or more items in the list first.")
            return []
        if not deep_ok and any(i.deep for i in sel):
            messagebox.showinfo(APP, "Deep-scan results aren't normal files yet. Use Recover to… to copy them out.")
            return []
        if not real_ok and any(not i.deep for i in sel):
            messagebox.showinfo(APP, "Recover is for deep-scan results. Normal files can be moved instead.")
            return []
        return sel

    def open_location(self):
        sel = self.need()
        if sel:
            os.system(f'explorer /select,"{sel[0].path}"')

    def unhide(self):
        sel = self.need()
        n = 0
        for it in sel:
            p = long_path(it.path)
            a = ctypes.windll.kernel32.GetFileAttributesW(p)
            if a != 0xFFFFFFFF and ctypes.windll.kernel32.SetFileAttributesW(
                    p, a & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) or 0x80):
                n += 1
                it.why = ", ".join(w for w in it.why.split(", ") if w not in ("hidden", "system"))
        self.refresh()
        self.status.set(f"Unhid {n} item(s). They now show up normally in Explorer.")

    def move(self):
        sel = self.need()
        if not sel:
            return
        dest = filedialog.askdirectory(title="Move the selected items to…")
        if not dest:
            return
        n, errs = 0, []
        for it in sel:
            try:
                name = it.name.rstrip(" .") or "unnamed"
                target = os.path.join(dest, name)
                shutil.move(long_path(it.path), long_path(target))
                it.path = target
                it.name = name
                n += 1
            except Exception as e:
                errs.append(f"{it.name}: {e}")
        self.refresh()
        self.status.set(f"Moved {n} item(s) to {dest}.")
        if errs:
            messagebox.showwarning(APP, "Some items couldn't be moved:\n\n" + "\n".join(errs[:15]))

    def delete(self):
        sel = self.need()
        if not sel:
            return
        bin_ok = has_recycle_bin(sel[0].path)
        total = sum(i.size or 0 for i in sel)
        msg = (f"Delete {len(sel)} item(s), {human(total)}?\n\n" +
               ("They go to the Recycle Bin." if bin_ok else
                "This drive has no Recycle Bin, so they're deleted PERMANENTLY and can't be undone."))
        if not messagebox.askyesno(APP, msg, icon="warning"):
            return
        n, errs = 0, []
        for it in sel:
            p = long_path(it.path)
            try:
                if bin_ok and recycle(it.path):
                    pass
                elif it.is_dir:
                    shutil.rmtree(p)
                else:
                    os.chmod(p, stat.S_IWRITE)
                    os.remove(p)
                self.items.remove(it)
                n += 1
            except Exception as e:
                errs.append(f"{it.name}: {e}")
        self.refresh()
        self.status.set(f"Deleted {n} item(s).")
        if errs:
            messagebox.showwarning(APP, "Some items couldn't be deleted:\n\n" + "\n".join(errs[:15]))

    def recover(self):
        sel = self.need(deep_ok=True, real_ok=False)
        if not sel:
            return
        dest = filedialog.askdirectory(title="Recover to which folder? (must be a DIFFERENT drive)")
        if not dest:
            return
        src_drive = sel[0].deep[0].letter.upper()
        if os.path.splitdrive(os.path.abspath(dest))[0].rstrip(":").upper() == src_drive:
            messagebox.showerror(APP, "Recover to a different drive. Writing to the same card could overwrite "
                                      "the very files you're trying to get back.")
            return
        if self.busy():
            return
        files = [i for i in sel if not i.is_dir]

        def run():
            ok, errs = 0, []
            for k, it in enumerate(files):
                if self.stop.is_set():
                    break
                self.q.put(("prog", f"Recovering {k + 1} of {len(files)}: {it.name}", k / max(1, len(files))))
                try:
                    recover(it, dest)
                    ok += 1
                except Exception as e:
                    errs.append(f"{it.name}: {e}")
            note = f"Recovered {ok} file(s) to {dest}."
            if errs:
                note += f"  {len(errs)} failed (first: {errs[0][:120]})"
            note += "  Check they open: deleted files can be partly overwritten."
            self.q.put(("done", note))

        self.stop = threading.Event()
        self.worker = threading.Thread(target=run, daemon=True)
        self.worker.start()


def main():
    start, deep = None, False
    args = sys.argv[1:]
    if args and args[0] == "--deep":
        deep, args = True, args[1:]
    if args:
        start = args[0].strip('"')
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    App(root, start, deep)
    root.mainloop()


if __name__ == "__main__":
    main()
