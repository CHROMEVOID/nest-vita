#!/usr/bin/env python3
"""Nest Music: gets your music ready for Nest on the PS Vita.

1. Spotify / YouTube / SoundCloud playlist -> Nest playlist
   Paste a playlist (or album) link. Nest Music finds those songs in your
   music folder and writes Playlists/<name>.m3u8 plus the playlist's cover
   picture, so it shows up in Nest's Music tab with its real artwork.
   YouTube and SoundCloud work best with yt-dlp installed (py -m pip install yt-dlp):
   then every song comes through, not just the first 100 / first few.

2. Fix album and artist info
   Looks every song up (Deezer, free, no account) and fills in the right
   album, album artist, track number, disc, year and cover, so Nest sorts
   albums in the right order even when the files came from a downloader
   that numbered them by playlist position.

Double-click "Nest Music.bat", or:
    py nest_music.py playlist <Spotify / YouTube / SoundCloud link, or an Exportify .csv> --music <folder>
    py nest_music.py fix <folder> [--yes] [--dry-run]

Playlists with more than 100 songs: make a free app at
https://developer.spotify.com/dashboard and put its Client ID and Client
Secret on one line in spotify_keys.txt next to this file. Without it, the
first 100 songs come through (that's all Spotify's public page shows).
"""
import argparse
import base64
import csv
import json
import os
import re
import subprocess
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) NestMusic/1.0"
AUDIO = (".mp3", ".flac", ".ogg", ".oga", ".m4a", ".aac", ".wav")
CACHE_PATH = os.path.join(HERE, "nest_music_cache.json")


# ---------------------------------------------------------------- helpers

def need_mutagen():
    try:
        import mutagen  # noqa: F401
        return True
    except ImportError:
        pass
    print("One-time setup: installing 'mutagen' (reads and writes song tags)...")
    r = subprocess.run([sys.executable, "-m", "pip", "install", "--user", "--quiet", "mutagen"])
    if r.returncode != 0:
        print("Couldn't install mutagen. Run:  py -m pip install mutagen   and try again.")
        return False
    import importlib
    import site
    importlib.invalidate_caches()
    sys.path.append(site.getusersitepackages())
    try:
        import mutagen  # noqa: F401
        return True
    except ImportError:
        print("mutagen installed, but Python needs a restart. Please run me again.")
        return False


def http(url, headers=None, data=None, tries=4):
    for a in range(tries):
        try:
            req = urllib.request.Request(url, data=data, headers={"User-Agent": UA, **(headers or {})})
            with urllib.request.urlopen(req, timeout=25) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503) and a < tries - 1:
                time.sleep(1.5 * (a + 1))
                continue
            raise
        except (urllib.error.URLError, TimeoutError):
            if a < tries - 1:
                time.sleep(1.5 * (a + 1))
                continue
            raise


def http_json(url, headers=None, data=None):
    return json.loads(http(url, headers, data).decode("utf-8"))


_last = [0.0]


def deezer(path, **params):
    """Deezer's public API: 50 requests per 5 seconds, so pace ourselves."""
    wait = 0.11 - (time.time() - _last[0])
    if wait > 0:
        time.sleep(wait)
    _last[0] = time.time()
    q = urllib.parse.urlencode(params)
    d = http_json("https://api.deezer.com/" + path + ("?" + q if q else ""))
    if isinstance(d, dict) and d.get("error"):
        if d["error"].get("code") == 4:  # quota: slow down and retry
            time.sleep(5)
            return deezer(path, **params)
        return None
    return d


def norm(s):
    """Lower case, no accents, no (feat. ...), no '- Remastered 2011', no punctuation."""
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    s = re.sub(r"[\(\[][^\)\]]*(feat|ft\.|with |remaster|version|edit|mono|stereo|deluxe|bonus|explicit|clean)[^\)\]]*[\)\]]", " ", s)
    s = re.sub(r"\s-\s.*(remaster|version|edit|mono|stereo|live at|from |single).*$", " ", s)
    s = re.sub(r"\b(feat|ft)\b\.?.*$", " ", s)
    s = s.replace("&", " and ")
    s = re.sub(r"[^a-z0-9]+", " ", s)
    return " ".join(s.split())


def artist_set(s):
    parts = re.split(r"\s*(?:;|,|/| & | and | x | feat\.? | ft\.? | featuring )\s*", s or "", flags=re.I)
    return {norm(p) for p in parts if norm(p)}


def safe_name(s):
    s = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "", s).strip().rstrip(".")
    return s[:80] or "Playlist"


def load_cache():
    try:
        with open(CACHE_PATH, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {"search": {}, "album": {}}


def save_cache(c):
    try:
        tmp = CACHE_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(c, f)
        os.replace(tmp, CACHE_PATH)
    except OSError:
        pass


def ask(prompt, default=""):
    try:
        v = input(prompt).strip().strip('"')
    except EOFError:
        return default
    return v or default


def ask_folder(title):
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        p = filedialog.askdirectory(title=title)
        root.destroy()
        if p:
            return p
    except Exception:
        pass
    return ask(title + "\n> ")


# ---------------------------------------------------------------- your music

class Song:
    __slots__ = ("path", "title", "artist", "album", "aartist", "dur", "track", "has_art")

    def __init__(self, path):
        self.path = path
        self.title = self.artist = self.album = self.aartist = ""
        self.dur = 0
        self.track = 0
        self.has_art = False


def guess_from_name(path):
    """'03 - Artist - Title', 'Title - Artist' (downloaders), 'Artist - Title'."""
    name = os.path.splitext(os.path.basename(path))[0]
    name = re.sub(r"^\s*(?:track\s*)?\d{1,3}\s*[-._)\]]\s*", "", name, flags=re.I)
    name = re.sub(r"[\(\[][^\)\]]*(official|lyric|audio|video|kbps|hq|hd)[^\)\]]*[\)\]]", "", name, flags=re.I)
    name = name.replace("_", " ")
    parts = [p.strip() for p in name.split(" - ") if p.strip()]
    if len(parts) >= 2:
        return parts[-1], parts[0]  # title, artist
    return name.strip(), ""


def read_song(path):
    from mutagen import File
    s = Song(path)
    try:
        f = File(path, easy=True)
    except Exception:
        f = None
    if f is not None:
        tags = f.tags or {}

        def first(k):
            v = tags.get(k)
            return (v[0] if isinstance(v, list) and v else (v or "")) if v else ""
        s.title = str(first("title"))
        s.artist = str(first("artist")).replace(";", ", ")
        s.album = str(first("album"))
        s.aartist = str(first("albumartist"))
        try:
            s.track = int(str(first("tracknumber")).split("/")[0] or 0)
        except ValueError:
            s.track = 0
        try:
            s.dur = int((f.info.length or 0) * 1000)
        except Exception:
            s.dur = 0
    try:
        raw = File(path)
        if raw is not None and raw.tags is not None:
            keys = list(raw.tags.keys()) if hasattr(raw.tags, "keys") else []
            s.has_art = any(str(k).startswith("APIC") or k == "covr" or k == "metadata_block_picture" for k in keys)
        if raw is not None and getattr(raw, "pictures", None):
            s.has_art = True
    except Exception:
        pass
    if not s.title:
        s.title, a = guess_from_name(path)
        s.artist = s.artist or a
    return s


def scan_music(folder):
    files = []
    for root, dirs, names in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for n in names:
            if n.lower().endswith(AUDIO) and not n.startswith("._"):
                files.append(os.path.join(root, n))
    print(f"Reading {len(files)} songs in {folder} ...")
    songs = []
    for i, p in enumerate(files):
        songs.append(read_song(p))
        if (i + 1) % 200 == 0:
            print(f"  {i + 1} / {len(files)}")
    return songs


def match(track, songs, by_title):
    """Find the local file for a playlist song; videos that put the song first are tried both ways."""
    s = match_one(track, songs, by_title)
    if s is None and track.get("artist"):
        s = match_one({**track, "title": track["artist"], "artist": track["title"]}, songs, by_title)
    return s


def match_one(track, songs, by_title):
    """Title must match; then artist and length decide."""
    t = norm(track["title"])
    arts = artist_set(track["artist"])
    cands = by_title.get(t, [])
    if not cands:  # a looser try: one title contains the other
        cands = [s for s in songs if t and (t in norm(s.title) or norm(s.title) in t) and len(norm(s.title)) > 2]
    best, best_score = None, -1
    for s in cands:
        sa = artist_set(s.artist) | artist_set(s.aartist)
        score = 0
        if norm(s.title) == t:
            score += 3
        if arts & sa:
            score += 4
        elif any(a in b or b in a for a in arts for b in sa):
            score += 2
        elif arts and sa:
            score -= 3
        if track.get("dur") and s.dur:
            d = abs(track["dur"] - s.dur)
            score += 2 if d < 3000 else 0 if d < 10000 else -2
        if score > best_score:
            best, best_score = s, score
    if best is None:
        return None
    exact = norm(best.title) == t
    return best if best_score >= (3 if exact else 6) else None  # a looser title needs artist and length to agree


# ---------------------------------------------------------------- Spotify

def playlist_id(link):
    m = re.search(r"playlist[/:]([A-Za-z0-9]{10,})", link)
    return m.group(1) if m else None


def spotify_keys():
    p = os.path.join(HERE, "spotify_keys.txt")
    if not os.path.exists(p):
        return None
    parts = open(p, encoding="utf-8").read().split()
    return (parts[0], parts[1]) if len(parts) >= 2 else None


def from_web_api(pid, keys):
    tok = http_json("https://accounts.spotify.com/api/token", data=b"grant_type=client_credentials",
                    headers={"Authorization": "Basic " + base64.b64encode(f"{keys[0]}:{keys[1]}".encode()).decode(),
                             "Content-Type": "application/x-www-form-urlencoded"})["access_token"]
    h = {"Authorization": "Bearer " + tok}
    meta = http_json(f"https://api.spotify.com/v1/playlists/{pid}?fields=name,description,images", h)
    tracks, url = [], f"https://api.spotify.com/v1/playlists/{pid}/tracks?limit=100"
    while url:
        page = http_json(url, h)
        for it in page.get("items", []):
            tr = it.get("track") or {}
            if not tr or not tr.get("name"):
                continue
            tracks.append({"title": tr["name"], "artist": ", ".join(a["name"] for a in tr.get("artists", [])),
                           "album": (tr.get("album") or {}).get("name", ""), "dur": tr.get("duration_ms", 0)})
        url = page.get("next")
    imgs = meta.get("images") or []
    cover = max(imgs, key=lambda i: (i.get("width") or 0))["url"] if imgs else None
    desc = re.sub(r"<[^>]+>", "", meta.get("description") or "")
    return meta.get("name") or "Playlist", desc, cover, tracks


def from_embed(pid, kind="playlist"):
    html = http(f"https://open.spotify.com/embed/{kind}/{pid}").decode("utf-8", "replace")
    m = re.search(r'<script id="__NEXT_DATA__" type="application/json">(.*?)</script>', html, re.S)
    if not m:
        raise RuntimeError("Spotify's page didn't have the playlist in it. Is the playlist public?")
    e = json.loads(m.group(1))["props"]["pageProps"]["state"]["data"]["entity"]
    tracks = [{"title": t.get("title", ""), "artist": t.get("subtitle", "").replace(" ", " "), "album": e.get("name", "") if kind == "album" else "",
               "dur": t.get("duration", 0)} for t in e.get("trackList", []) if t.get("title")]
    srcs = (e.get("coverArt") or {}).get("sources") or []
    cover = max(srcs, key=lambda s: s.get("width") or 0)["url"] if srcs else None
    if not cover:
        try:
            o = http_json("https://open.spotify.com/oembed?url=" + urllib.parse.quote(f"https://open.spotify.com/{kind}/{pid}"))
            cover = o.get("thumbnail_url")
        except Exception:
            pass
    return e.get("name") or e.get("title") or "Playlist", "", cover, tracks


def from_csv(path):
    """An Exportify / TuneMyMusic style export."""
    tracks = []
    with open(path, encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            low = {k.lower().strip(): v for k, v in row.items() if k}
            title = low.get("track name") or low.get("name") or low.get("title") or ""
            artist = low.get("artist name(s)") or low.get("artist name") or low.get("artist") or low.get("artists") or ""
            dur = low.get("track duration (ms)") or low.get("duration (ms)") or "0"
            if title:
                tracks.append({"title": title, "artist": artist.replace(";", ", "), "album": low.get("album name") or low.get("album") or "",
                               "dur": int(float(dur or 0))})
    return os.path.splitext(os.path.basename(path))[0], "", None, tracks


def resolve_short(src):
    """spotify.link / spoti.fi / on.soundcloud.com / youtu.be: follow them to the real page."""
    host = urllib.parse.urlparse(src if "://" in src else "https://" + src).netloc.lower()
    if not any(h in host for h in ("spotify.link", "spoti.fi", "on.soundcloud.com", "goo.gl", "app.link")):
        return src
    try:
        req = urllib.request.Request(src if "://" in src else "https://" + src, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=20) as r:
            final, body = r.geturl(), r.read(400000).decode("utf-8", "replace")
        if "open.spotify.com" in final or "soundcloud.com" in final and "on.soundcloud" not in final:
            return final
        m = re.search(r"https://open\.spotify\.com/[a-z-]*/?(playlist|album)/[A-Za-z0-9]+", body)
        if m:
            return m.group(0)
    except Exception:
        pass
    return src


def ytdlp_cmd():
    """yt-dlp, if it's on this PC (as a program or a Python package)."""
    import shutil
    exe = shutil.which("yt-dlp")
    if exe:
        return [exe]
    try:
        import importlib.util
        if importlib.util.find_spec("yt_dlp"):
            return [sys.executable, "-m", "yt_dlp"]
    except Exception:
        pass
    return None


def ytdlp_json(url, flat=True):
    cmd = ytdlp_cmd()
    if not cmd:
        return None
    args = cmd + ["-J", "--no-warnings", "--ignore-errors", "--skip-download"] + (["--flat-playlist"] if flat else []) + [url]
    try:
        r = subprocess.run(args, capture_output=True, timeout=900, env={**os.environ, "PYTHONIOENCODING": "utf-8"})
        out = r.stdout.decode("utf-8", "replace").strip()
        return json.loads(out) if out.startswith("{") else None
    except Exception:
        return None


JUNK_VIDEO = re.compile(r"\s*[\(\[](?:official|lyric|lyrics|audio|video|visuali[sz]er|music video|mv|hd|hq|4k|explicit|clean|"
                        r"full song|color coded|with lyrics|official audio|official video|official music video)[^\)\]]*[\)\]]", re.I)


def split_video_title(title, channel):
    """'Artist - Song (Official Video)' -> (Song, Artist); otherwise the channel is the artist."""
    t = JUNK_VIDEO.sub("", title or "")
    t = re.sub(r"\s*\|.*$", "", t)  # "Song | Album 2020"
    t = re.sub(r"\s+(official\s+(music\s+)?(video|audio)|lyric\s+video|lyrics|music\s+video|visuali[sz]er)\s*$", "", t, flags=re.I)
    t = t.replace("\u2013", "-").replace("\u2014", "-").strip()
    ch = re.sub(r"\s*-\s*Topic$|VEVO$|\s*Official$|\s*Music$", "", (channel or "").strip(), flags=re.I).strip()
    m = re.match(r"^(.+?)\s+[-~]\s+(.+)$", t)
    if m:
        artist, song = m.group(1).strip(), m.group(2).strip()
    else:
        artist, song = ch, t
    song = song.strip(' "\'')
    return song, artist


def from_youtube(url):
    m = re.search(r"[?&]list=([A-Za-z0-9_-]+)", url)
    if not m:
        raise RuntimeError("That YouTube link isn't a playlist (it needs list=... in it).")
    url = "https://www.youtube.com/playlist?list=" + m.group(1)
    d = ytdlp_json(url)
    if d and d.get("entries"):
        tracks = []
        for e in d["entries"]:
            if not e or not e.get("title") or e.get("title") in ("[Private video]", "[Deleted video]"):
                continue
            song, artist = split_video_title(e["title"], e.get("channel") or e.get("uploader"))
            tracks.append({"title": song, "artist": artist, "album": "", "dur": int((e.get("duration") or 0) * 1000)})
        thumbs = d.get("thumbnails") or []
        cover = None
        if thumbs:
            cover = max(thumbs, key=lambda t: (t.get("width") or 0) * (t.get("height") or 0) or (t.get("preference") or 0)).get("url")
        if not cover and d["entries"] and d["entries"][0].get("id"):
            cover = f"https://i.ytimg.com/vi/{d['entries'][0]['id']}/hqdefault.jpg"
        return d.get("title") or "YouTube playlist", d.get("description") or "", cover, tracks
    # no yt-dlp: read the public page (the first 100 videos)
    html = http(url, headers={"Accept-Language": "en-US,en"}).decode("utf-8", "replace")
    mm = re.search(r"var ytInitialData = (\{.*?\});</script>", html, re.S)
    if not mm:
        raise RuntimeError("YouTube didn't show that playlist. Is it public or unlisted?")
    data = json.loads(mm.group(1))
    found = []

    def walk(o):
        if isinstance(o, dict):
            lv = o.get("lockupViewModel")
            if isinstance(lv, dict) and lv.get("contentType") == "LOCKUP_CONTENT_TYPE_VIDEO":
                title = (((lv.get("metadata") or {}).get("lockupMetadataViewModel") or {}).get("title") or {}).get("content")
                if title:
                    found.append((lv.get("contentId"), title))
            pv = o.get("playlistVideoRenderer")
            if isinstance(pv, dict):
                title = ((pv.get("title") or {}).get("runs") or [{}])[0].get("text")
                ch = ((pv.get("shortBylineText") or {}).get("runs") or [{}])[0].get("text")
                if title:
                    found.append((pv.get("videoId"), title, ch, int(pv.get("lengthSeconds") or 0)))
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(data)
    tracks = []
    for f in found:
        song, artist = split_video_title(f[1], f[2] if len(f) > 2 else "")
        tracks.append({"title": song, "artist": artist, "album": "", "dur": (f[3] * 1000) if len(f) > 3 else 0})
    name = re.search(r'"title":"([^"]+)","description"', html)
    name = json.loads('"' + name.group(1) + '"') if name else "YouTube playlist"
    cover = f"https://i.ytimg.com/vi/{found[0][0]}/hqdefault.jpg" if found and found[0][0] else None
    if len(tracks) >= 100:
        print("  YouTube's page shows the first 100 videos. Install yt-dlp (py -m pip install yt-dlp) for the rest.")
    return name, "", cover, tracks


def from_soundcloud(url):
    url = url.split("?")[0]
    if "/sets/" not in url:
        raise RuntimeError("That SoundCloud link isn't a playlist (it needs /sets/ in it).")
    name, cover, tracks, total = "SoundCloud playlist", None, [], 0
    try:
        html = http(url).decode("utf-8", "replace")
        m = re.search(r"window\.__sc_hydration = (\[.*?\]);</script>", html, re.S)
        for x in json.loads(m.group(1)) if m else []:
            if x.get("hydratable") == "playlist":
                pl = x["data"]
                name = pl.get("title") or name
                art = pl.get("artwork_url") or ((pl.get("tracks") or [{}])[0].get("artwork_url"))
                cover = art.replace("-large.", "-t500x500.") if art else None
                total = pl.get("track_count") or len(pl.get("tracks") or [])
                for t in pl.get("tracks") or []:
                    if t.get("title"):
                        song, artist = split_video_title(t["title"], (t.get("user") or {}).get("username"))
                        tracks.append({"title": song, "artist": artist, "album": "", "dur": t.get("duration") or 0})
    except Exception as e:
        print(f"  (SoundCloud page: {e})")
    if total and len(tracks) < total:
        print(f"  SoundCloud only shows {len(tracks)} of {total} songs on the page; asking yt-dlp for the rest (takes a moment) ...")
        d = ytdlp_json(url, flat=False)
        if d and d.get("entries"):
            tracks = []
            for e in d["entries"]:
                if e and e.get("title"):
                    song, artist = split_video_title(e["title"], e.get("uploader"))
                    tracks.append({"title": song, "artist": artist, "album": "", "dur": int((e.get("duration") or 0) * 1000)})
            name = d.get("title") or name
        elif not ytdlp_cmd():
            print("  Install yt-dlp (py -m pip install yt-dlp) to get every song from SoundCloud playlists.")
    if not tracks:
        raise RuntimeError("Couldn't read that SoundCloud playlist. Is it public?")
    return name, "", cover, tracks


def get_playlist(src):
    if os.path.isfile(src) and src.lower().endswith(".csv"):
        return from_csv(src)
    src = resolve_short(src.strip())
    low = src.lower()
    if "youtube.com" in low or "youtu.be" in low:
        return from_youtube(src)
    if "soundcloud.com" in low:
        return from_soundcloud(src)
    am = re.search(r"album[/:]([A-Za-z0-9]{10,})", src)
    if am:  # a Spotify album works too
        return from_embed(am.group(1), "album")
    pid = playlist_id(src)
    if not pid:
        raise RuntimeError("That doesn't look like a playlist link (Spotify, YouTube or SoundCloud).")
    keys = spotify_keys()
    if keys:
        try:
            return from_web_api(pid, keys)
        except Exception as e:
            print(f"  (Spotify API keys didn't work: {e}; using the public page instead)")
    name, desc, cover, tracks = from_embed(pid)
    if len(tracks) >= 100:
        print("  Spotify's public page only shows the first 100 songs. For the rest, add spotify_keys.txt (see the top of nest_music.py).")
    return name, desc, cover, tracks


def make_playlist(src, music, songs=None):
    print(f"\nGetting the playlist from Spotify ...")
    name, desc, cover, tracks = get_playlist(src)
    print(f"  \"{name}\": {len(tracks)} songs")
    if songs is None:
        songs = scan_music(music)
    by_title = {}
    for s in songs:
        by_title.setdefault(norm(s.title), []).append(s)
    pl_dir = os.path.join(music, "Playlists")
    os.makedirs(pl_dir, exist_ok=True)
    base = safe_name(name)
    found, missing = 0, []
    lines = ["#EXTM3U", f"#PLAYLIST:{name}"]
    if desc:
        lines.append("#NEST-DESC:" + " ".join(desc.split())[:300])
    for t in tracks:
        s = match(t, songs, by_title)
        lines.append(f"#EXTINF:{int((t.get('dur') or 0) / 1000)},{t['artist']} - {t['title']}")
        if s:
            found += 1
            lines.append(os.path.relpath(s.path, pl_dir).replace("\\", "/"))
        else:
            missing.append(t)
            # kept as a hint: Nest matches it later if you add the song
            lines.append("missing/" + safe_name(f"{t['artist']} - {t['title']}") + ".mp3")
    with open(os.path.join(pl_dir, base + ".m3u8"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if cover:
        try:
            img = http(cover)
            ext = ".png" if img[:4] == b"\x89PNG" else ".jpg"
            for old in (".jpg", ".png"):
                op = os.path.join(pl_dir, base + old)
                if old != ext and os.path.exists(op):
                    os.remove(op)
            with open(os.path.join(pl_dir, base + ext), "wb") as f:
                f.write(img)
        except Exception as e:
            print(f"  (couldn't get the cover: {e})")
    miss_path = os.path.join(pl_dir, base + " - missing songs.txt")
    if missing:
        with open(miss_path, "w", encoding="utf-8") as f:
            f.write(f"Songs from \"{name}\" that aren't in your music folder yet:\n\n")
            for t in missing:
                f.write(f"{t['artist']} - {t['title']}\n")
    elif os.path.exists(miss_path):
        os.remove(miss_path)
    print(f"  Matched {found} of {len(tracks)} songs -> Playlists/{base}.m3u8")
    if missing:
        print(f"  {len(missing)} aren't in your folder (listed in \"{base} - missing songs.txt\"); they appear in Nest once you add them.")
    return songs


# ---------------------------------------------------------------- fixing tags

def lookup(s, cache):
    """Find the song on Deezer: (track, album) dicts, or None if not sure."""
    title, artist = s.title, (s.aartist or s.artist).split(",")[0].split(";")[0]
    if not title or not artist:
        return None
    key = norm(artist) + "|" + norm(title) + "|" + norm(s.album)
    hit = cache["search"].get(key)
    if hit is None:
        items = []
        if s.album:  # the album the file already says comes first
            res = deezer("search", q=f'artist:"{artist}" track:"{title}" album:"{s.album}"', limit=10) or {}
            items = res.get("data") or []
        if not items:
            res = deezer("search", q=f'artist:"{artist}" track:"{title}"', limit=15) or {}
            items = res.get("data") or []
        if not items:
            res = deezer("search", q=f"{artist} {title}", limit=15) or {}
            items = res.get("data") or []
        hit = [{"id": i["id"], "title": i.get("title", ""), "artist": (i.get("artist") or {}).get("name", ""),
                "album": (i.get("album") or {}).get("id"), "album_title": (i.get("album") or {}).get("title", ""),
                "dur": (i.get("duration") or 0) * 1000} for i in items]
        cache["search"][key] = hit
    t, arts = norm(title), artist_set(s.artist) | artist_set(s.aartist)
    best, best_score = None, 0
    for i in hit:
        score = 0
        it = norm(i["title"])
        if it == t:
            score += 4
        elif t and (t in it or it in t):
            score += 2
        else:
            continue
        ia = artist_set(i["artist"])
        if ia & arts:
            score += 4
        elif any(a in b or b in a for a in ia for b in arts):
            score += 2
        else:
            continue
        if s.dur and i["dur"]:
            d = abs(s.dur - i["dur"])
            score += 2 if d < 3000 else -1 if d < 15000 else -4
        if s.album:
            fa, da = norm(s.album), norm(i["album_title"])
            if fa == da:
                score += 3
            elif fa and da and (fa in da or da in fa):
                score += 1
            else:
                continue  # never move a song to a different album than the one it says
        if score > best_score:
            best, best_score = i, score
    if not best or best_score < 7:
        return None
    aid = str(best["album"])
    al = cache["album"].get(aid)
    if al is None:
        a = deezer(f"album/{aid}") or {}
        if not a:
            return None
        al = {"title": a.get("title", ""), "artist": (a.get("artist") or {}).get("name", ""),
              "date": a.get("release_date", ""), "cover": a.get("cover_xl") or a.get("cover_big") or "",
              "genre": ((a.get("genres") or {}).get("data") or [{}])[0].get("name", ""),
              "tracks": {str(tr["id"]): [tr.get("track_position", 0), tr.get("disk_number", 1)]
                         for tr in (a.get("tracks") or {}).get("data", [])}}
        if al["tracks"] and not any(v[0] for v in al["tracks"].values()):
            # the album list doesn't carry positions: number them in order
            al["tracks"] = {k: [n + 1, 1] for n, k in enumerate(al["tracks"])}
        cache["album"][aid] = al
    pos = al["tracks"].get(str(best["id"]))
    if not pos:
        tr = deezer(f"track/{best['id']}") or {}
        pos = [tr.get("track_position", 0), tr.get("disk_number", 1)]
    return best, al, pos


def write_tags(s, best, al, pos, covers, dry):
    from mutagen import File
    changes = []
    f = File(s.path, easy=True)
    if f is None:
        return changes
    if f.tags is None:
        try:
            f.add_tags()
        except Exception:
            return changes

    def cur(k):
        v = f.tags.get(k)
        return str(v[0]) if v else ""

    def put(k, v):
        if v and cur(k) != str(v) and norm(cur(k)) != norm(str(v)):
            changes.append(f"{k}: {cur(k) or '-'} -> {v}")
            if not dry:
                f.tags[k] = str(v)
    if not cur("album"):
        put("album", al["title"])
    if not cur("albumartist"):
        put("albumartist", al["artist"])
    if pos and pos[0]:
        put("tracknumber", str(pos[0]))
        put("discnumber", str(pos[1] or 1))
    if al["date"][:4].isdigit() and al["date"][:4] != "0000":
        put("date", al["date"][:4])
    if not cur("genre") and al.get("genre"):
        put("genre", al["genre"])
    if changes and not dry:
        f.save()
    if not s.has_art and al["cover"]:
        changes.append("cover added")
        if not dry:
            img = covers.get(al["cover"])
            if img is None:
                try:
                    img = http(al["cover"])
                except Exception:
                    img = b""
                covers[al["cover"]] = img
            if img:
                add_cover(s.path, img)
    return changes


def add_cover(path, img):
    from mutagen import File
    mime = "image/png" if img[:4] == b"\x89PNG" else "image/jpeg"
    low = path.lower()
    if low.endswith(".mp3"):
        from mutagen.id3 import APIC, ID3, ID3NoHeaderError
        try:
            t = ID3(path)
        except ID3NoHeaderError:
            t = ID3()
        t.add(APIC(encoding=3, mime=mime, type=3, desc="Cover", data=img))
        t.save(path)
    elif low.endswith(".flac"):
        from mutagen.flac import FLAC, Picture
        f = FLAC(path)
        p = Picture()
        p.type, p.mime, p.data = 3, mime, img
        f.add_picture(p)
        f.save()
    elif low.endswith((".m4a", ".mp4", ".aac")):
        from mutagen.mp4 import MP4, MP4Cover
        f = MP4(path)
        f["covr"] = [MP4Cover(img, imageformat=MP4Cover.FORMAT_PNG if mime == "image/png" else MP4Cover.FORMAT_JPEG)]
        f.save()
    elif low.endswith((".ogg", ".oga")):
        from mutagen.flac import Picture
        f = File(path)
        p = Picture()
        p.type, p.mime, p.data = 3, mime, img
        f["metadata_block_picture"] = [base64.b64encode(p.write()).decode("ascii")]
        f.save()


def artist_pictures(music, songs, cache):
    """Artists/<name>/artist.jpg: Nest shows these on artist pages."""
    names = {}
    for s in songs:
        a = (s.aartist or s.artist).split(",")[0].split(";")[0].strip()
        if a:
            names[a] = names.get(a, 0) + 1
    out = os.path.join(music, "Artists")
    made = 0
    for a in sorted(names):
        dest = os.path.join(out, safe_name(a), "artist.jpg")
        if os.path.exists(dest):
            continue
        key = "artist|" + norm(a)
        url = cache["search"].get(key)
        if url is None:
            r = deezer("search/artist", q=a, limit=3) or {}
            url = ""
            for it in r.get("data") or []:
                if norm(it.get("name")) == norm(a) and it.get("picture_xl") and "/artist//" not in it["picture_xl"]:
                    url = it["picture_xl"]
                    break
            cache["search"][key] = url
        if url:
            try:
                img = http(url)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(img)
                made += 1
            except Exception:
                pass
    return made


def fix_folder(music, songs=None, yes=False, dry=False):
    if songs is None:
        songs = scan_music(music)
    if not songs:
        print("No songs found in that folder.")
        return songs
    if not dry and not yes:
        a = ask(f"\nUpdate album / artist / track number / year / cover tags in up to {len(songs)} songs? (y/n) ", "n")
        if a.lower() not in ("y", "yes"):
            dry = True
            print("OK, just showing what would change.")
    cache = load_cache()
    covers = {}
    changed = unsure = 0
    t0 = time.time()
    for i, s in enumerate(songs):
        try:
            r = lookup(s, cache)
        except Exception as e:
            print(f"  ! {os.path.basename(s.path)}: {e}")
            r = None
        if not r:
            unsure += 1
        else:
            ch = write_tags(s, *r, covers, dry)
            if ch:
                changed += 1
                if dry or changed <= 30:
                    print(f"  {os.path.basename(s.path)}: " + "; ".join(ch))
        if (i + 1) % 50 == 0:
            save_cache(cache)
            print(f"  ... {i + 1} / {len(songs)} ({int(time.time() - t0)} s)")
    save_cache(cache)
    print(f"\n{'Would update' if dry else 'Updated'} {changed} songs. {unsure} couldn't be matched for sure, so they were left alone.")
    if not dry:
        n = artist_pictures(music, songs, cache)
        save_cache(cache)
        if n:
            print(f"Saved {n} artist pictures in Artists/ (Nest shows them on artist pages).")
    return songs


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    p1 = sub.add_parser("playlist", help="make a Nest playlist from a Spotify link or .csv")
    p1.add_argument("source", nargs="+")
    p1.add_argument("--music", help="your music folder")
    p2 = sub.add_parser("fix", help="fix album / artist / track info")
    p2.add_argument("folder")
    p2.add_argument("--yes", action="store_true")
    p2.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):  # song names in any language: never crash printing them
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    if not need_mutagen():
        return 1
    try:
        if args.cmd == "playlist":
            music = args.music or ask_folder("Pick your music folder (the one you copy to the Vita)")
            songs = None
            for src in args.source:
                songs = make_playlist(src, music, songs)
        elif args.cmd == "fix":
            fix_folder(args.folder, yes=args.yes, dry=args.dry_run)
        else:
            print("Nest Music\n----------\n"
                  "1  Make Nest playlists from Spotify links\n"
                  "2  Fix album, artist, track numbers and covers in a music folder\n"
                  "3  Both\n")
            c = ask("Pick 1, 2 or 3: ", "1")
            music = ask_folder("Pick your music folder (the one you copy to the Vita)")
            if not music or not os.path.isdir(music):
                print("No folder picked.")
                return 1
            songs = None
            if c in ("2", "3"):
                songs = fix_folder(music)
                if c == "3":
                    songs = scan_music(music)  # read the fixed tags
            if c in ("1", "3"):
                print("\nPaste playlist links (Spotify, YouTube or SoundCloud, or an Exportify .csv), one per line. Empty line when done.")
                while True:
                    src = ask("link> ")
                    if not src:
                        break
                    try:
                        songs = make_playlist(src, music, songs)
                    except Exception as e:
                        print(f"  Couldn't do that one: {e}")
            print("\nDone. Copy the music folder (with Playlists/ and Artists/) to ux0:music on your Vita, then open Music in Nest.")
    except KeyboardInterrupt:
        print("\nStopped.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
