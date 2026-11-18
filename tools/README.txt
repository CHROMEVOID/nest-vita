Nest PC tools (Windows) - double-click the .bat files.
They need Python 3 from https://www.python.org/downloads/ (tick "Add python.exe to PATH").

  Nest Metadata.bat   Posters, logos, backdrops, descriptions and episode names from TMDB for a
                      whole folder. Asks when it isn't sure, remembers its matches, and lists
                      anything it couldn't find. Force a match with {tmdb-12345} in a folder name.
                      Needs a free TMDB API key (themoviedb.org > Settings > API). It asks once
                      and saves it only on this PC (tmdb_key.txt next to this file).
  Nest Music.bat      Paste a Spotify, YouTube or SoundCloud playlist link (or a Spotify album):
                      it finds those songs in your music folder and writes a Nest playlist with the
                      real cover, in a Playlists folder, plus a list of songs you don't have yet.
                      You can paste several links, one after another.
                      It can also fix album, artist, track numbers, years and covers in your song
                      tags, so albums play in the right order. No account needed.
                      It never moves, renames or downloads songs.
                      - Spotify playlists over 100 songs: make a free app at
                        developer.spotify.com/dashboard and put its Client ID and Client Secret on one
                        line in spotify_keys.txt next to this file.
                      - YouTube / SoundCloud: install yt-dlp (py -m pip install yt-dlp) to get every
                        song, not just the first ones.
                      Then copy your music folder (with Playlists/ and Artists/) to ux0:music.
  Nest PC Server.bat  Lets Nest on your Vita play videos from a folder on this PC over Wi-Fi.
                      Nest > Settings > Library > Watch from your PC, then type the address shown.
  Nest Finder.bat     Finds hidden, oddly named and deleted files on a folder or memory card,
                      and lets you move, delete, unhide or recover them.

Full guide: https://github.com/CHROMEVOID/nest-vita
