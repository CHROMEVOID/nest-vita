"""Nest Metadata - real posters, logos, backdrops, descriptions and episode names for Nest.

Run it on a PC against the folders you copy to your PS Vita. It saves small
files next to your videos; Nest picks them up on its next scan, fully offline.

    Easiest: double-click "Nest Metadata.bat" (paste your TMDB key once, pick a folder).

    Show folder:   poster.jpg  fanart.jpg  logo.png  nest.json (with every episode's name)
    Season folder: poster.jpg (that season's own poster)
    Movie folder:  poster.jpg  fanart.jpg  logo.png  nest.json
    Loose movie:   <file>-poster.jpg  <file>-fanart.jpg  <file>-logo.png  <file>.json

What it does well:
  * Matches carefully: file name AND folder name, year, original and alternative
    titles, popularity. When two results are close it asks you (or skips with --auto).
  * Remembers matches: the next run on the same folder is instant, and a fix you
    make sticks. Force a match with "{tmdb-1234}" in a folder name or a nest-id.txt
    file containing 1234 (or a TMDB link).
  * Only ever upgrades: a bigger poster or fuller description replaces the old one,
    never the other way round (--force replaces everything).
  * Fast: works on several titles at once.
  * Writes a report of anything it couldn't find.

Usage:
    py nest_meta.py "E:/Vita/Videos" [--key KEY] [--auto] [--force] [--refresh] [--lang en-US] [--dry-run]

Get a free TMDB API key at https://www.themoviedb.org/settings/api
"""
import argparse
import concurrent.futures as cf
import difflib
import json
import math
import os
import re
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request

API = "https://api.themoviedb.org/3"
IMG = "https://image.tmdb.org/t/p/"
VIDEO_EXT = {".mp4", ".m4v", ".mkv", ".avi", ".mov", ".wmv", ".ts", ".m2ts", ".webm", ".mpg", ".mpeg"}
JUNK = re.compile(
    r"\b(2160p|1080p|1080i|720p|576p|480p|360p|4k|uhd|blu-?ray|bdrip|brrip|br-rip|web-?dl|webrip|web|hdtv|hdrip|dvdrip|dvdscr|"
    r"x26[45]|h\.?26[45]|hevc|avc|xvid|divx|10bit|8bit|hdr10?|dv|aac[0-9.]*|e?ac3|ddp?[0-9.]*|dts(-hd)?|truehd|atmos|"
    r"remux|proper|repack|extended|unrated|remastered|imax|amzn|nf|dsnp|hmax|atvp|hulu|pcok|mkv|mp4|vita|psvita|converted|"
    r"subbed|dubbed|dual[ .-]?audio|multi|complete|season[ ._-]*pack|internal|limited)\b",
    re.I,
)
EPISODE = re.compile(r"\b[sS](\d{1,2})[ ._-]?[eE](\d{1,3})\b|\b(\d{1,2})x(\d{2,3})\b")
SEASON_DIR = re.compile(r"^(season|series|staffel|saison|temporada)[ ._-]*\d+$|^s\d{1,2}$|^specials?$", re.I)
YEAR = re.compile(r"[\[(. _-]((?:19|20)\d{2})[\])(. _-]|[\[(. _-]((?:19|20)\d{2})$")
FORCED_ID = re.compile(r"[\[{(]\s*(?:tmdb(?:id)?)[-=: ]?(\d+)\s*[\]})]", re.I)

LANG = "en-US"
DRY = False
print_lock = threading.Lock()


def say(*a):
    with print_lock:
        print(*a, flush=True)


# ---------------------------------------------------------------- names

def clean(name):
    base = os.path.splitext(name)[0] if os.path.splitext(name)[1].lower() in VIDEO_EXT else name
    base = FORCED_ID.sub(" ", base)
    base = re.sub(r"\[[^\]]*\]|\{[^}]*\}", " ", base)
    year = None
    m = YEAR.search(" " + base + " ")
    if m:
        year = int(m.group(1) or m.group(2))
    s = re.sub(r"[._]", " ", base)
    cut = len(s)
    for rx in (EPISODE, JUNK, re.compile(r"\b[sS]\d{1,2}\b"), re.compile(r"\bseason\s*\d", re.I)):
        mm = rx.search(s)
        if mm and mm.start() > 0:
            cut = min(cut, mm.start())
    if year:
        ym = re.search(r"[( ]%d\b" % year, s)
        if ym and ym.start() > 0:
            cut = min(cut, ym.start())
    em = EPISODE.search(s)
    if em and em.start() == 0:
        cut = 0  # the name starts with the episode code (S01E01.mp4): no title in it
    t = re.sub(r"\(\s*\)", "", s[:cut]).strip(" -_:(")
    t = re.sub(r"\s+", " ", t)
    return t, year


def norm(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    s = s.replace("&", " and ")
    s = re.sub(r"^(the|a|an)\s+", "", s)
    s = re.sub(r"[^a-z0-9]+", " ", s)
    return " ".join(s.split())


def similar(a, b):
    a, b = norm(a), norm(b)
    if not a or not b:
        return 0.0
    if a == b:
        return 1.0
    r = difflib.SequenceMatcher(None, a, b).ratio()
    if a in b or b in a:
        r = max(r, 0.82 * min(len(a), len(b)) / max(len(a), len(b)) + 0.15)
    return r


# ---------------------------------------------------------------- TMDB

_cache = {}
_cache_lock = threading.Lock()


def get(path, key, **params):
    params.setdefault("language", LANG)
    params = {k: v for k, v in params.items() if v is not None}
    ck = path + "?" + urllib.parse.urlencode(sorted(params.items()))
    with _cache_lock:
        if ck in _cache:
            return _cache[ck]
    headers = {"User-Agent": "nest-meta/2", "Accept": "application/json"}
    if len(key) > 40:  # a v4 "Read Access Token" (long, starts with eyJ...)
        headers["Authorization"] = "Bearer " + key
    else:  # a v3 "API Key"
        params["api_key"] = key
    url = API + path + "?" + urllib.parse.urlencode(params)
    for attempt in range(5):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=20) as r:
                d = json.load(r)
                with _cache_lock:
                    _cache[ck] = d
                return d
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            if e.code == 401:
                return None
            time.sleep(1.2 * (attempt + 1))  # 429 / 5xx
        except Exception:
            time.sleep(1.2 * (attempt + 1))
    say("   ! TMDB didn't answer:", path)
    return None


def _pixels(data_or_path):
    """Image area in pixels (Pillow if installed, else file size as a stand-in)."""
    try:
        from PIL import Image
        import io
        im = Image.open(io.BytesIO(data_or_path) if isinstance(data_or_path, bytes) else data_or_path)
        return im.size[0] * im.size[1]
    except Exception:
        try:
            return len(data_or_path) if isinstance(data_or_path, bytes) else os.path.getsize(data_or_path)
        except OSError:
            return 0


def download(url, dest, force, what="image"):
    """Save url to dest; an existing file is only replaced by a better (bigger) one, or with --force."""
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "nest-meta/2"}), timeout=40) as r:
                data = r.read()
            break
        except Exception as e:
            if attempt == 2:
                say("   ! download failed:", os.path.basename(dest), e)
                return False
            time.sleep(1.5)
    if len(data) < 500:
        return False
    if os.path.exists(dest) and not force:
        if _pixels(data) <= _pixels(dest) * 1.05:
            return False
        say("   + better", what + ":", os.path.basename(dest))
    if DRY:
        return True
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)
    return True


def pick_image(items, langs=("en", None), prefer_textless=False):
    """Best image: wanted language, then votes, then size."""
    best, best_k = None, None
    for it in items or []:
        lang = it.get("iso_639_1")
        if lang not in langs and lang is not None:
            continue
        if prefer_textless:
            lk = 0 if lang is None else 1
        else:
            lk = langs.index(lang) if lang in langs else len(langs)
        k = (lk, -(it.get("vote_average") or 0) * math.log10(2 + (it.get("vote_count") or 0)), -(it.get("width") or 0))
        if best_k is None or k < best_k:
            best, best_k = it, k
    return best["file_path"] if best else None


# ---------------------------------------------------------------- matching

def candidates(kind, title, year, key):
    found = {}
    tries = []
    if year:
        tries.append({"query": title, ("year" if kind == "movie" else "first_air_date_year"): year})
    tries.append({"query": title})
    # "Show Name US" / "Show (UK)" style suffixes, and a trailing number
    t2 = re.sub(r"\b(us|uk|au|nz|ca)\b$", "", title, flags=re.I).strip()
    if t2 and t2 != title:
        tries.append({"query": t2})
    for p in tries:
        r = get(f"/search/{kind}", key, include_adult="false", **p) or {}
        for it in (r.get("results") or [])[:10]:
            found.setdefault(it["id"], it)
        if len(found) >= 8 and year:
            break
    return list(found.values())


def score(it, kind, titles, year):
    name = it.get("title") or it.get("name") or ""
    orig = it.get("original_title") or it.get("original_name") or ""
    sim = max([similar(t, name) for t in titles] + [similar(t, orig) * 0.97 for t in titles] + [0])
    date = it.get("release_date") or it.get("first_air_date") or ""
    y = int(date[:4]) if date[:4].isdigit() else None
    s = sim * 10
    if year and y:
        s += 3 if y == year else 1.2 if abs(y - year) == 1 else -2.5
    pop = math.log10(1 + (it.get("vote_count") or 0))
    s += min(pop, 4) * 0.6 + min(math.log10(1 + (it.get("popularity") or 0)), 3) * 0.3
    if not it.get("poster_path"):
        s -= 1.5
    return s, sim


def ask_choice(label, ranked):
    with print_lock:
        print(f"\n   Which one is \"{label}\"?")
        for n, (sc, sim, it) in enumerate(ranked[:5], 1):
            name = it.get("title") or it.get("name")
            date = (it.get("release_date") or it.get("first_air_date") or "")[:4]
            ov = (it.get("overview") or "").replace("\n", " ")
            print(f"     {n}) {name} ({date or '?'})  {ov[:70]}{'...' if len(ov) > 70 else ''}")
        print("     0) none of these (skip)")
        try:
            v = input("   Pick a number [1]: ").strip()
        except EOFError:
            v = ""
    if v == "0":
        return None
    if v.isdigit() and 1 <= int(v) <= min(5, len(ranked)):
        return ranked[int(v) - 1][2]
    return ranked[0][2]


ask_lock = threading.Lock()


def find(kind, names, year, key, auto, label):
    """Search under every name we have; returns (result, confident)."""
    pool = {}
    for n in names:
        if n:
            for it in candidates(kind, n, year, key):
                pool.setdefault(it["id"], it)
    if not pool:
        return None
    ranked = sorted(((*score(it, kind, names, year), it) for it in pool.values()), key=lambda x: -x[0])
    top = ranked[0]
    close = len(ranked) > 1 and ranked[1][0] > top[0] - 1.2 and ranked[1][1] > 0.75
    if top[1] >= 0.86 and not close:
        return top[2]
    if top[1] < 0.55 and auto:
        return None
    if auto:
        return top[2] if top[1] >= 0.7 else None
    with ask_lock:
        return ask_choice(label, ranked)


def forced_id(folder, filename=None):
    for s in (filename or "", os.path.basename(folder)):
        m = FORCED_ID.search(s)
        if m:
            return int(m.group(1))
    p = os.path.join(folder, "nest-id.txt")
    if os.path.exists(p):
        m = re.search(r"(\d+)", open(p, encoding="utf-8", errors="replace").read())
        if m:
            return int(m.group(1))
    return None


def remembered(json_path, kind):
    try:
        with open(json_path, encoding="utf-8") as f:
            j = json.load(f)
        if j.get("tmdb_id") and j.get("tmdb_kind", kind) == kind:
            return j["tmdb_id"]
    except (OSError, ValueError):
        pass
    return None


# ---------------------------------------------------------------- writing

def better_meta(path, new, force):
    try:
        with open(path, encoding="utf-8") as f:
            old = json.load(f)
    except (OSError, ValueError):
        return True, None
    if force:
        return True, old
    if old.get("tmdb_id") and new.get("tmdb_id") and old["tmdb_id"] != new["tmdb_id"]:
        return True, old
    def richness(m):
        return (sum(1 for k in ("overview", "genres", "rating", "year", "tmdb_id", "runtime", "tagline", "cast", "certification") if m.get(k))
                + len(m.get("episodes") or {}) * 0.01)
    if richness(new) > richness(old) or len(new.get("overview") or "") > len(old.get("overview") or "") + 20:
        return True, old
    return False, old


def save_json(path, meta, force):
    ok, old = better_meta(path, meta, force)
    if not ok:
        return False
    if old and old.get("title_locked"):  # you renamed it yourself: keep your name
        meta["title"] = old.get("title", meta["title"])
        meta["title_locked"] = True
    if DRY:
        return True
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)
    return True


def certification(kind, tid, key):
    if kind == "movie":
        d = get(f"/movie/{tid}/release_dates", key) or {}
        for country in ("US", "GB", "AU"):
            for r in d.get("results", []):
                if r.get("iso_3166_1") == country:
                    for rd in r.get("release_dates", []):
                        if rd.get("certification"):
                            return rd["certification"]
    else:
        d = get(f"/tv/{tid}/content_ratings", key) or {}
        for country in ("US", "GB", "AU"):
            for r in d.get("results", []):
                if r.get("iso_3166_1") == country and r.get("rating"):
                    return r["rating"]
    return ""


def write_title(kind, tid, folder, prefix, key, force, owns_folder, seasons_wanted=None):
    d = get(f"/{kind}/{tid}", key, append_to_response="credits,images", include_image_language="en,null")
    if not d:
        return None
    title = d.get("title") or d.get("name")
    date = d.get("release_date") or d.get("first_air_date") or ""
    imgs = d.get("images") or {}
    names = ("poster.jpg", "fanart.jpg", "logo.png") if owns_folder else (prefix + "-poster.jpg", prefix + "-fanart.jpg", prefix + "-logo.png")
    jobs = []
    poster = pick_image(imgs.get("posters"), ("en", None)) or d.get("poster_path")
    backdrop = pick_image(imgs.get("backdrops"), (None, "en"), prefer_textless=True) or d.get("backdrop_path")
    logo = pick_image(imgs.get("logos"), ("en", None))
    if poster:
        jobs.append((IMG + "w500" + poster, os.path.join(folder, names[0]), "poster"))
    if backdrop:
        jobs.append((IMG + "w1280" + backdrop, os.path.join(folder, names[1]), "backdrop"))
    if logo:
        jobs.append((IMG + "w500" + logo, os.path.join(folder, names[2]), "logo"))
    for url, dest, what in jobs:
        download(url, dest, force, what)
    cast = [c.get("name") for c in (d.get("credits") or {}).get("cast", [])[:8] if c.get("name")]
    crew = (d.get("credits") or {}).get("crew", [])
    directors = [c["name"] for c in crew if c.get("job") == "Director"][:2]
    runtime = d.get("runtime") or ((d.get("episode_run_time") or [None])[0])
    meta = {
        "title": title,
        "original_title": d.get("original_title") or d.get("original_name") or "",
        "year": int(date[:4]) if date[:4].isdigit() else None,
        "overview": d.get("overview") or "",
        "tagline": d.get("tagline") or "",
        "genres": [g["name"] for g in d.get("genres", [])],
        "rating": round(d.get("vote_average") or 0, 1),
        "certification": certification(kind, tid, key),
        "runtime": runtime,
        "cast": cast,
        "directors": directors or [c.get("name") for c in d.get("created_by", [])][:2],
        "collection": (d.get("belongs_to_collection") or {}).get("name", ""),
        "tmdb_id": tid,
        "tmdb_kind": kind,
    }
    if kind == "tv":
        meta["seasons"] = d.get("number_of_seasons")
        meta["status"] = d.get("status")
        eps = {}
        nums = [s.get("season_number") for s in d.get("seasons", []) if s.get("season_number") is not None]
        if seasons_wanted:
            nums = [n for n in nums if n in seasons_wanted] or nums
        for n in nums:
            sd = get(f"/tv/{tid}/season/{n}", key) or {}
            for e in sd.get("episodes", []):
                if e.get("name") and e.get("episode_number") is not None:
                    eps[f"S{n:02d}E{e['episode_number']:02d}"] = e["name"]
        meta["episodes"] = eps
    jp = os.path.join(folder, "nest.json") if owns_folder else os.path.join(folder, prefix + ".json")
    if owns_folder or kind == "movie":
        save_json(jp, meta, force)
    return title, date[:4], d


def season_poster(tid, season, folder, key, force):
    sd = get(f"/tv/{tid}/season/{season}", key)
    if sd and sd.get("poster_path"):
        if download(IMG + "w500" + sd["poster_path"], os.path.join(folder, "poster.jpg"), force, "season poster"):
            say(f"   OK  season {season} poster")


def season_of(folder, files):
    b = os.path.basename(folder)
    m = re.search(r"(?:season|series|staffel|saison|temporada)[ ._-]*(\d{1,2})", b, re.I) or re.search(r"(?:^|[^a-z0-9])[sS](\d{1,2})(?=$|[^0-9]|[eE]\d)", b)
    if m:
        return int(m.group(1))
    seasons = {int(e.group(1) or e.group(3)) for e in (EPISODE.search(f) for f in files) if e}
    return seasons.pop() if len(seasons) == 1 else None


# ---------------------------------------------------------------- the walk

def plan(folder):
    """Look at the whole folder first: which shows (with their season folders) and which movies."""
    shows, movies = {}, []
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d.lower() not in ("extras", "featurettes", "sample", "samples", "subs", "subtitles", "behind the scenes")
                   and not d.startswith(".")]
        vids = [f for f in files if os.path.splitext(f)[1].lower() in VIDEO_EXT and "sample" not in f.lower()]
        if not vids:
            continue
        base = os.path.basename(root)
        episodic = [f for f in vids if EPISODE.search(f)] or (vids if SEASON_DIR.match(base) else [])
        if episodic:
            show_root = os.path.dirname(root) if SEASON_DIR.match(base) else root
            fname_title, _ = clean(episodic[0])
            folder_title, fyear = clean(os.path.basename(show_root))
            if SEASON_DIR.match(base) is None and re.search(r"\b[sS]\d{1,2}\b", base):
                show_root = os.path.dirname(root) if fname_title.lower() != folder_title.lower() else root
            if os.path.normpath(show_root) == os.path.normpath(root) and season_of(root, []) is not None \
                    and os.path.normpath(root) != os.path.normpath(folder):
                show_root = os.path.dirname(root)  # "Show.S10..." is one season: the show lives one level up
            if os.path.normpath(show_root) == os.path.normpath(folder) and os.path.normpath(root) != os.path.normpath(folder):
                show_root = root  # a lone "Show.S02" folder at the top: it is the show's folder (never the library root)
            st, sy = clean(os.path.basename(show_root))
            title = fname_title or folder_title
            k = norm(title) or norm(st)
            sh = shows.setdefault(k, {"names": [], "year": None, "root": show_root, "seasons": {}, "files": 0})
            for n in (fname_title, folder_title, st):
                if n and n not in sh["names"]:
                    sh["names"].append(n)
            sh["year"] = sh["year"] or fyear or sy
            sh["files"] += len(episodic)
            if os.path.normpath(root) != os.path.normpath(folder):
                n = season_of(root, episodic)
                if n is not None and os.path.normpath(root) != os.path.normpath(show_root):
                    sh["seasons"][root] = n
            continue
        for v in vids:
            title, year = clean(v)
            ftitle, fyear = clean(base)
            owns = len(vids) == 1 and os.path.normpath(root) != os.path.normpath(folder)
            names = [t for t in (title, ftitle if owns else None) if t]
            if not names:
                names = [ftitle or os.path.splitext(v)[0]]
            movies.append({"file": v, "root": root, "names": names, "year": year or (fyear if owns else None), "owns": owns})
    return shows, movies


def do_show(sh, key, a, report):
    label = sh["names"][0]
    tid = forced_id(sh["root"]) or (None if a.refresh else remembered(os.path.join(sh["root"], "nest.json"), "tv"))
    if not tid:
        it = find("tv", sh["names"], sh["year"], key, a.auto, label)
        if not it:
            report["missing"].append(f"show: {label}  ({sh['root']})")
            say(f"show:  {label}   ? not found")
            return
        tid = it["id"]
    r = write_title("tv", tid, sh["root"], None, key, a.force, True, None)
    if not r:
        report["missing"].append(f"show: {label}  ({sh['root']})")
        return
    title, year, d = r
    for root, n in sorted(sh["seasons"].items(), key=lambda x: x[1]):
        season_poster(tid, n, root, key, a.force)
    neps = sum(1 for _ in (d.get("seasons") or []))
    say(f"show:  {title} ({year})  OK  {len(sh['seasons']) or ''}{' season posters, ' if sh['seasons'] else ''}episode names for {neps} seasons")
    report["done"] += 1


def do_movie(mv, key, a, report):
    label = mv["names"][0]
    prefix = os.path.splitext(mv["file"])[0]
    jp = os.path.join(mv["root"], "nest.json") if mv["owns"] else os.path.join(mv["root"], prefix + ".json")
    tid = forced_id(mv["root"] if mv["owns"] else "", mv["file"]) or (None if a.refresh else remembered(jp, "movie"))
    if not tid:
        it = find("movie", mv["names"], mv["year"], key, a.auto, label)
        if not it:
            report["missing"].append(f"movie: {label}  ({os.path.join(mv['root'], mv['file'])})")
            say(f"movie: {label}   ? not found")
            return
        tid = it["id"]
    r = write_title("movie", tid, mv["root"], prefix, key, a.force, mv["owns"])
    if r:
        say(f"movie: {r[0]} ({r[1]})  OK")
        report["done"] += 1


# ---------------------------------------------------------------- UI bits

def ask_box(title, prompt):
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
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        v = filedialog.askdirectory(title="Choose the folder with your videos")
        root.destroy()
        return v
    except Exception:
        print("Drag your video folder into this window (or type its path), then press Enter.")
        return input("> ").strip().strip('"')


def main():
    global LANG, DRY
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?")
    ap.add_argument("--key", default=os.environ.get("TMDB_API_KEY"))
    ap.add_argument("--force", action="store_true", help="replace existing artwork and info")
    ap.add_argument("--refresh", action="store_true", help="search again even for titles matched before")
    ap.add_argument("--auto", action="store_true", help="never ask: skip anything unsure")
    ap.add_argument("--lang", default="en-US", help="language for titles and descriptions, e.g. en-US, fr-FR")
    ap.add_argument("--dry-run", action="store_true", help="show what would happen, change nothing")
    ap.add_argument("--workers", type=int, default=6)
    a = ap.parse_args()
    LANG, DRY = a.lang, a.dry_run

    key_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tmdb_key.txt")
    if not a.key and os.path.exists(key_file):
        a.key = open(key_file, encoding="utf-8").read().strip()
    if not a.key:
        a.key = ask_box("Nest Metadata", "Paste your TMDB key here\n(the 'API Key' or the long 'API Read Access Token').\nIt is saved, so you only do this once.")
        if not a.key:
            sys.exit("No key entered.")
        with open(key_file, "w", encoding="utf-8") as f:
            f.write(a.key)
    if not get("/configuration", a.key, language=None):
        if os.path.exists(key_file):
            os.remove(key_file)
        sys.exit("TMDB didn't accept that key. Run me again and paste it carefully (no spaces).")
    if not a.folder:
        a.folder = ask_folder()
    if not a.folder or not os.path.isdir(a.folder):
        sys.exit("That folder wasn't found: %s" % a.folder)
    if not sys.stdin or not sys.stdin.isatty():
        a.auto = True
    t0 = time.time()
    print("Looking through", a.folder)
    shows, movies = plan(a.folder)
    print(f"Found {len(shows)} show{'s' if len(shows) != 1 else ''} and {len(movies)} movie{'s' if len(movies) != 1 else ''}.\n")
    report = {"done": 0, "missing": []}
    with cf.ThreadPoolExecutor(max_workers=max(1, a.workers)) as ex:
        futs = [ex.submit(do_show, sh, a.key, a, report) for sh in shows.values()]
        futs += [ex.submit(do_movie, mv, a.key, a, report) for mv in movies]
        for f in cf.as_completed(futs):
            try:
                f.result()
            except Exception as e:
                say("   ! something went wrong:", e)
    took = int(time.time() - t0)
    print(f"\nDone in {took // 60}m {took % 60}s: {report['done']} titles updated.")
    rp = os.path.join(a.folder, "Nest Metadata - not found.txt")
    if report["missing"]:
        print(f"{len(report['missing'])} couldn't be matched. They're listed in \"{os.path.basename(rp)}\".")
        print("Tip: rename the folder to the real title (with the year), or add {tmdb-12345} to its name, then run again.")
        if not DRY:
            with open(rp, "w", encoding="utf-8") as f:
                f.write("Nest Metadata couldn't match these. Rename them to the real title (and year),\n"
                        "or put {tmdb-12345} in the folder name (the number from the TMDB page), then run again.\n\n")
                f.write("\n".join(sorted(report["missing"])) + "\n")
    elif os.path.exists(rp) and not DRY:
        os.remove(rp)


if __name__ == "__main__":
    main()
