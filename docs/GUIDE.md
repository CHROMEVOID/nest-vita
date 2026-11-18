# Nest — full guide

[← Back to the overview](../README.md)

**On this page:** [Install](#install-in-2-minutes) · [Video format](#getting-your-videos-ready) · [Posters & logos](#posters-and-descriptions) · [Watch from your PC](#watch-from-your-pc-over-wi-fi) · [PC tools](#the-pc-tools) · [Controls](#controls) · [Troubleshooting](#troubleshooting) · [Building](#building-from-source)

### Everything Nest can do
**Watching**
- Finds every video on your memory card and groups it into **Movies** and **Shows → Seasons → Episodes** by itself — it understands names like `Show.S02E05.720p.mp4`, `Show - 2x05`, `Season 2/05 - Title.mp4` and cleans off the junk (`1080p`, `WEB-DL`, `x264`…).
- **Continue watching**, resume points, "Up next" with auto-play of the next episode.
- **Skip intro**: mark a show's intro once (START while watching) and press **R** to skip it on every episode. Mark the credits and *Up next* appears as soon as they start.
- **Picture size per show** (Fit / Fill / Stretch with SELECT) — remembered for each show.
- Subtitles (`.srt` / `.vtt` next to the video), adjustable size.
- Ratings, favorites, collections, "More like this", and search across titles, genres, years and episode names.

**Reading**
- Comics & manga (`.cbz`, `.cbr`), `.pdf`, `.epub` and `.txt`.
- Manga mode (right-to-left), **two-page spreads**, a **long-strip** view for webtoons, fit width / height — remembered per book.
- Text books get size, spacing, margins and a warm reading light.

**Everything else**
- Six themes and 16 animated pixel-art "lofi" scenes that live behind your library, with optional ambient sound.
- A **Secret** tab behind a PIN for things you'd rather keep out of sight.
- **History & your year** — what you watched and read, hours, streaks and your top titles.
- **Storage** — what's taking space on the card, with one-tap "delete what I've watched".
- **Delete from Vita** — removes the actual files (and their artwork), not just the library entry. Always asks first.
- **Watch from your PC over Wi-Fi** *(beta)* — stream your PC's video folder without copying anything.
- **Find info in the folder** — pulls posters, descriptions and episode names out of `.nfo`/artwork files you already have (Kodi / Jellyfin / Plex / tinyMediaManager style).


---

## Install in 2 minutes

You need a Vita (or PS TV) with **HENkaku / Ensō** homebrew enabled and **VitaShell**. If you don't have that yet, follow the guide at **[vita.hacks.guide](https://vita.hacks.guide)** first.

1. Download **`Nest.vpk`** from the [latest release](https://github.com/CHROMEVOID/nest-vita/releases/latest).
2. Copy it to your Vita — the easiest way is VitaShell's **USB** or **FTP** mode (press SELECT in VitaShell) and drop it into `ux0:/`.
3. In VitaShell, highlight `Nest.vpk`, press **✕** and choose **Install**. Say yes to the extended permissions question (Nest needs it to read your media folders).
4. Open **Nest** from the home screen. The first-run screen asks which folder your videos/books are in — pick it (for example `ux0:/video`) and let it scan.

Updating later: install the new `Nest.vpk` the same way. Your library, progress and settings are kept (they live in `ux0:data/Nest`).

---

## Getting your videos ready

The Vita's hardware decoder plays **MP4 with H.264 video and AAC audio**. Nest shows everything it finds, but files in other formats (MKV, HEVC/x265, AVI…) get a small "needs converting" badge instead of playing.

**Converting with HandBrake (free, Windows/Mac/Linux):**
1. Open [HandBrake](https://handbrake.fr), drop in your file or folder.
2. Preset: **General → Fast 720p30** (or *Fast 480p30* to save space).
3. *Summary* tab → Format **MP4**. *Video* tab → Encoder **H.264 (x264)**, Profile **Main** or **High**, Level **4.0** or lower. *Audio* → **AAC**.
4. Start. Copy the results to your Vita.

720p is the sweet spot — the Vita's screen is 960×544, so anything bigger only costs space.

**Suggested layout** (anything works, this just makes it tidiest):
```
ux0:/video/
├── Movies/
│   └── Night Train (2019)/
│       ├── Night Train (2019).mp4
│       └── poster.jpg              ← optional
└── Shows/
    └── Lantern Street/
        ├── poster.jpg  fanart.jpg  ← optional
        ├── Season 1/
        │   ├── Lantern Street S01E01.mp4
        │   └── Lantern Street S01E01.srt
        └── Season 2/ …
```
Books and comics can sit in the same folders or their own (`ux0:/books`).

---

## Posters and descriptions

Nest uses whatever artwork is next to your files. There are three ways to get it — pick one:

### 1. Already have Kodi / Jellyfin / Plex files? → "Find info in the folder"
On any show or movie, open **⋯ → Find info in the folder**. Nest searches the folder you added for
- `tvshow.nfo`, `movie.nfo` or `<name>.nfo` (description, year, genres, rating, title),
- `poster.jpg` / `folder.jpg` / `cover.jpg`, `fanart.jpg` / `backdrop.jpg`, `logo.png` / `clearlogo.png` — in the show's folder, in a folder named after it, or named after it (`Lantern Street-poster.jpg`),
- `<episode>.nfo` next to each episode (real episode names),

and applies it. What it finds is remembered, so rescans keep it.

### 2. Nothing yet? → **Nest Metadata** on your PC (automatic)
Downloads posters, backdrops, logos and descriptions from TMDB for a whole folder at once.

1. Install [Python 3](https://www.python.org/downloads/) (tick **"Add python.exe to PATH"**).
2. Get a **free TMDB API key**: make an account at [themoviedb.org](https://www.themoviedb.org/signup) → *Settings → API → Create → Developer*. Copy the **API Key** (or the long *Read Access Token*).
3. Download **`Nest-PC-Tools.zip`** from the [release](https://github.com/CHROMEVOID/nest-vita/releases/latest), unzip it, and double-click **`Nest Metadata.bat`**. Paste your key once (it's saved only on your PC, in `tools/tmdb_key.txt`).
4. Pick your video folder. It writes `poster.jpg`, `fanart.jpg`, `logo.png` and `nest.json` (with every episode's real name) next to your files, plus each season folder's own poster. It only ever swaps in something *better* than what's there.
   - Not sure which film you meant? It shows the top matches and lets you pick. Matches are remembered, so the next run is instant.
   - Still wrong? Put `{tmdb-12345}` in the folder name (the number from the TMDB page) and run it again.
   - Anything it couldn't find is listed in `Nest Metadata - not found.txt`.
5. Copy the folder to your Vita (or **Settings → Library → Rescan everything**).

### 3. By hand
Drop a `poster.jpg` (and optionally `fanart.jpg`, `logo.png`) into a show's or movie's folder, or name it after a loose file: `Night Train (2019)-poster.jpg`. You can also choose any image on the card from **⋯ → Choose poster image…**.

---

## Music

Nest has a **Music** tab, with playlists, albums, artists, songs, search and a proper Now Playing screen.

1. Put your music in **`ux0:music`** (or any folder you've added to Nest). MP3, FLAC, OGG, M4A and WAV all work.
2. Open **Music**. Nest reads the tags (and the cover art inside the files) and sorts everything into albums and artists. Songs with no tags are worked out from the file and folder names, track numbers included (`03 - Song.mp3`, `1-04 Song.flac`, `Artist - Album (2004)/...`).
3. Pick something. It keeps playing while you browse, and the little bar at the bottom takes you back to it.

**Now Playing:** L / R skip, SQUARE pauses, TRIANGLE opens the queue, START has the song options, and **SELECT turns the screen off** while the music keeps going (any button wakes it). The volume slider goes past 100% for quiet songs. There's a sleep timer too.

**Spotify, YouTube and SoundCloud playlists:** run **Nest Music** on your PC (in Nest-PC-Tools.zip), paste a playlist or album link, and it finds those songs in your music folder and makes a Nest playlist with the real cover. It can also fix album, artist, track numbers, years and covers in your song tags, so albums play in the right order.

---

## Watch from your PC over Wi-Fi

*(Beta.)* Stream videos straight from your PC — no copying, no card space.

1. On the PC: unzip **Nest-PC-Tools.zip** and double-click **`Nest PC Server.bat`** (needs [Python 3](https://www.python.org/downloads/)).
2. Click **Browse…**, pick your video folder, press **Start**. The window shows an address like `192.168.1.20`.
3. If Windows asks whether Python may use the network, allow **Private networks** only.
4. On the Vita: **Nest → Settings → Library → Watch from your PC (Wi-Fi)** and type that address.

Your PC's shows appear in Nest like any other folder, with resume points and subtitles. The PC must stay on and awake, files still need to be MP4/H.264, and both devices need to be on the same home network. Only the folder you pick is shared, read-only, and only inside your network — close the window to stop sharing.

---

## The PC tools

All in **Nest-PC-Tools.zip** (Windows, need Python 3; double-click the `.bat`):

| Tool | What it's for |
|---|---|
| **Nest Metadata** | Posters, logos, backdrops and descriptions from TMDB for a whole folder. |
| **Nest Music** | Turns Spotify, YouTube and SoundCloud playlist links into Nest playlists (with covers), and fixes album / artist / track info in your songs. No account needed. |
| **Nest PC Server** | Share a PC folder so Nest can stream it over Wi-Fi. |
| **Nest Finder** | Point it at a folder or memory card to find hidden, oddly-named and *deleted* files (it can read deleted entries straight off an exFAT card and recover them to another drive). Move, delete, unhide or search them. Deep scans need "Run as administrator". |

---

## Controls

| | Library | Watching | Reading |
|---|---|---|---|
| **✕** | Open / select | Play / pause, choose | Show / hide the menu |
| **○** | Back | Back | Back |
| **△** | Options for the title | Focus mode (controls stay hidden) | Manga mode (right-to-left) on/off |
| **□** | Favorite | Play / pause | Change page view (remembered per book) |
| **← / →** | Move | Back / forward 10 s | Previous / next page |
| **L / R** | Previous / next tab | Back 30 s / **skip intro** (or forward 30 s) | Previous / next page |
| **SELECT** | — | Picture size: Fit / Fill / Stretch (per show) | — |
| **START** | — | Mark intro & credits for this show | Show / hide the menu |
| Touch | Everything is tappable and swipeable | Tap for controls; double-tap the sides to skip 10 s | Tap the edges to turn, drag to scroll |

---

## Troubleshooting

**"Needs converting" on a video** — it isn't MP4/H.264/AAC. See [Getting your videos ready](#getting-your-videos-ready).

**A show is split in two / an episode is in the wrong place** — name episodes with `S01E02` (or put them in `Season 1/` folders) and **Settings → Library → Rescan everything**. You can also rename the title from **⋯ → Edit title**.

**No posters** — see [Posters and descriptions](#posters-and-descriptions). Or turn on **Settings → Library → Thumbnails from videos** to use a frame from the video.

**Black bars / squashed picture** — press **SELECT** while watching to switch Fit / Fill / Stretch; Nest remembers it for that show.

**Watch from PC can't connect** — check the PC and Vita are on the same Wi-Fi, the server window says *Sharing*, and Windows Firewall allowed Python on *Private* networks. Type just the address shown (e.g. `192.168.1.20`).

**Start over** — **Settings → Library → Clear library**, or delete `ux0:data/Nest` with VitaShell (this also resets settings and the Secret PIN).

Found a bug? [Open an issue](https://github.com/CHROMEVOID/nest-vita/issues) with what you did and what happened.

---

## Building from source

Nest is C with [VitaSDK](https://vitasdk.org) and vita2d. Every push is built by GitHub Actions (see `.github/workflows/build.yml`) — the VPK appears under the run's *Artifacts*.

Locally, with VitaSDK installed:
```bash
vdpm libwebp jansson unrar libjpeg-turbo freetype libpng zlib libvita2d
mkdir -p third_party/sqlite && cd third_party/sqlite \
  && curl -sSLO https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip \
  && unzip -j sqlite-amalgamation-3460100.zip && cd ../..
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
```
The name parser and scanner have host tests in `tests/` (`cc -I src tests/test_parse.c src/parse.c && ./a.out`).

## Credits

Made by **chromevoid** — follow along on TikTok **[@chr0m3void](https://www.tiktok.com/@chr0m3void)**.

Fonts: Inter, DM Serif Display, Space Mono (SIL Open Font License). Libraries: vita2d, SQLite, libjpeg-turbo, libwebp, libpng, FreeType, zlib, UnRAR. Metadata in Nest Metadata comes from [TMDB](https://www.themoviedb.org) (this product uses the TMDB API but is not endorsed or certified by TMDB).

Nest is a media player for files you own. It doesn't download, unlock or share any content.

Released under the [MIT License](../LICENSE).
