"""nest_meta - fetch posters, backdrops, clear logos and descriptions for Nest.

Run this on a PC against the same folders you copy to your Vita. It writes
small files next to your media; Nest picks them up on the next scan, fully
offline:

    Easiest: double-click "Nest Metadata.bat" (paste your key once, then drag a folder in).

    Movie folder:  poster.jpg  fanart.jpg  logo.png  nest.json
    Loose movie:   <file>-poster.jpg  <file>-fanart.jpg  <file>-logo.png
    Show folder:   poster.jpg  fanart.jpg  logo.png  nest.json

Usage:
    python nest_meta.py "E:/Vita/Movies" --key YOUR_TMDB_API_KEY
    (or set the TMDB_API_KEY environment variable)

Get a free API key (v3) at https://www.themoviedb.org/settings/api
Existing files are never overwritten unless you pass --force.
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

API = "https://api.themoviedb.org/3"
IMG = "https://image.tmdb.org/t/p/"
VIDEO_EXT = {".mp4", ".m4v", ".mkv", ".avi", ".mov", ".wmv", ".ts", ".m2ts", ".webm", ".mpg", ".mpeg"}
JUNK = re.compile(
    r"\b(2160p|1080p|1080i|720p|576p|480p|4k|uhd|blu-?ray|bdrip|brrip|br-rip|web-?dl|webrip|web|hdtv|hdrip|dvdrip|"
    r"x26[45]|h\.?26[45]|hevc|avc|xvid|divx|10bit|8bit|hdr10?|dv|aac[0-9.]*|e?ac3|ddp?[0-9.]*|dts(-hd)?|truehd|atmos|"
    r"remux|proper|repack|extended|unrated|remastered|imax|amzn|nf|dsnp|hmax|atvp|mkv|mp4)\b",
    re.I,
)
EPISODE = re.compile(r"\b[sS](\d{1,2})[ ._-]?[eE](\d{1,3})\b|\b(\d{1,2})x(\d{2,3})\b")
SEASON_DIR = re.compile(r"^(season|series|staffel|saison|temporada)[ ._-]*\d+$|^s\d{1,2}$|^specials?$", re.I)
YEAR = re.compile(r"[\[(. _-]((?:19|20)\d{2})[\])(. _-]|[\[(. _-]((?:19|20)\d{2})$")


def clean(name):
    base = os.path.splitext(name)[0] if os.path.splitext(name)[1].lower() in VIDEO_EXT else name
    base = re.sub(r"\[[^\]]*\]|\{[^}]*\}", " ", base)
    year = None
    m = YEAR.search(" " + base + " ")
    if m:
        year = int(m.group(1) or m.group(2))
    s = re.sub(r"[._]", " ", base)
    cut = len(s)
    for rx in (EPISODE, JUNK, re.compile(r"\b[sS]\d{1,2}\b")):
        mm = rx.search(s)
        if mm and mm.start() > 0:
            cut = min(cut, mm.start())
    if year:
        ym = re.search(r"[( ]%d\b" % year, s)
        if ym and ym.start() > 0:
            cut = min(cut, ym.start())
    t = re.sub(r"\(\s*\)", "", s[:cut]).strip(" -_:(")
    t = re.sub(r"\s+", " ", t)
    return t, year


def get(path, key, **params):
    headers = {"User-Agent": "nest-meta", "Accept": "application/json"}
    if len(key) > 40:  # a v4 "Read Access Token" (long, starts with eyJ...)
        headers["Authorization"] = "Bearer " + key
    else:  # a v3 "API Key" (32 letters and numbers)
        params["api_key"] = key
    url = API + path + "?" + urllib.parse.urlencode(params)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=20) as r:
                return json.load(r)
        except Exception as e:  # rate limits / hiccups
            if attempt == 2:
                print("   ! request failed:", e)
                return None
            time.sleep(1.5 * (attempt + 1))


def download(url, dest, force):
    if os.path.exists(dest) and not force:
        return
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "nest-meta"}), timeout=30) as r:
            data = r.read()
        with open(dest, "wb") as f:
            f.write(data)
    except Exception as e:
        print("   ! download failed:", e)


def best_logo(images):
    logos = [l for l in (images or {}).get("logos", []) if l.get("iso_639_1") in ("en", None)]
    logos.sort(key=lambda l: (l.get("iso_639_1") != "en", -l.get("vote_average", 0)))
    return logos[0]["file_path"] if logos else None


def write_meta(kind, info, folder, prefix, key, force, owns_folder):
    tid = info["id"]
    details = get(f"/{kind}/{tid}", key) or info
    images = get(f"/{kind}/{tid}/images", key, include_image_language="en,null")
    title = details.get("title") or details.get("name")
    date = details.get("release_date") or details.get("first_air_date") or ""
    names = (
        ("poster.jpg", "fanart.jpg", "logo.png")
        if owns_folder
        else (prefix + "-poster.jpg", prefix + "-fanart.jpg", prefix + "-logo.png")
    )
    if details.get("poster_path"):
        download(IMG + "w342" + details["poster_path"], os.path.join(folder, names[0]), force)
    if details.get("backdrop_path"):
        download(IMG + "w780" + details["backdrop_path"], os.path.join(folder, names[1]), force)
    logo = best_logo(images)
    if logo:
        download(IMG + "w300" + logo, os.path.join(folder, names[2]), force)
    if owns_folder:
        meta = {
            "title": title,
            "year": int(date[:4]) if date[:4].isdigit() else None,
            "overview": details.get("overview") or "",
            "genres": [g["name"] for g in details.get("genres", [])],
            "rating": round(details.get("vote_average") or 0, 1),
            "tmdb_id": tid,
        }
        p = os.path.join(folder, "nest.json")
        if force or not os.path.exists(p):
            with open(p, "w", encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False, indent=1)
    print(f"   OK  {title} ({date[:4]})")


def search(kind, title, year, key):
    params = {"query": title, "include_adult": "false"}
    if year:
        params["year" if kind == "movie" else "first_air_date_year"] = year
    r = get(f"/search/{kind}", key, **params)
    res = (r or {}).get("results") or []
    if not res and year:
        r = get(f"/search/{kind}", key, query=title)
        res = (r or {}).get("results") or []
    return res[0] if res else None


def ask_box(title, prompt):
    """A small pop-up box for the key; falls back to typing in the window."""
    try:
        import tkinter as tk
        from tkinter import simpledialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        v = simpledialog.askstring(title, prompt, parent=root)
        root.destroy()
        return (v or "").strip().strip('"')
    except Exception:
        print(prompt)
        return input("> ").strip().strip('"')


def ask_folder():
    """A normal Windows folder picker; falls back to typing a path."""
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        v = filedialog.askdirectory(title="Choose the folder with your videos and books")
        root.destroy()
        return v
    except Exception:
        print("Drag your video folder into this window (or type its path), then press Enter.")
        return input("> ").strip().strip('"')


def main():
    # Windows consoles often can't print symbols like the tick mark: never crash over it
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?")
    ap.add_argument("--key", default=os.environ.get("TMDB_API_KEY"))
    ap.add_argument("--force", action="store_true", help="overwrite existing artwork")
    a = ap.parse_args()

    # the key is remembered in tmdb_key.txt next to this script after the first run
    key_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tmdb_key.txt")
    if not a.key and os.path.exists(key_file):
        a.key = open(key_file, encoding="utf-8").read().strip()
    if not a.key:
        a.key = ask_box("Nest Metadata", "Paste your TMDB key here\n(the 'API Key' or the long 'API Read Access Token').\nIt is saved, so you only do this once.")
        if not a.key:
            sys.exit("No key entered.")
        with open(key_file, "w", encoding="utf-8") as f:
            f.write(a.key)
    test = get("/configuration", a.key)
    if not test:
        if os.path.exists(key_file):
            os.remove(key_file)
        sys.exit("TMDB didn't accept that key. Run me again and paste it carefully (no spaces).")
    if not a.folder:
        a.folder = ask_folder()
    if not a.folder or not os.path.isdir(a.folder):
        sys.exit("That folder wasn't found: %s" % a.folder)
    print("Looking through", a.folder)

    shows_done = set()
    for root, dirs, files in os.walk(a.folder):
        dirs[:] = [d for d in dirs if d.lower() not in ("extras", "featurettes", "sample", "samples", "subs")]
        vids = [f for f in files if os.path.splitext(f)[1].lower() in VIDEO_EXT and "sample" not in f.lower()]
        if not vids:
            continue
        episodic = [f for f in vids if EPISODE.search(f)] or (vids if SEASON_DIR.match(os.path.basename(root)) else [])
        if episodic:
            show_root = os.path.dirname(root) if SEASON_DIR.match(os.path.basename(root)) else root
            fname_title, _ = clean(episodic[0])
            folder_title, fyear = clean(os.path.basename(show_root))
            if SEASON_DIR.match(os.path.basename(root)) is None and re.search(r"\b[sS]\d{1,2}\b", os.path.basename(root)):
                show_root = os.path.dirname(root) if fname_title.lower() != folder_title.lower() else root
            title = fname_title or folder_title
            k = title.lower()
            if k in shows_done:
                continue
            shows_done.add(k)
            print("show:", title)
            info = search("tv", title, fyear, a.key)
            if info:
                write_meta("tv", info, show_root, None, a.key, a.force, True)
            else:
                print("   ? not found")
            continue
        for v in vids:
            title, year = clean(v)
            if not title:
                title, year = clean(os.path.basename(root))
            print("movie:", title, year or "")
            info = search("movie", title, year, a.key)
            if not info:
                print("   ? not found")
                continue
            owns = len(vids) == 1 and root != a.folder
            write_meta("movie", info, root, os.path.splitext(v)[0], a.key, a.force, owns)


if __name__ == "__main__":
    main()
