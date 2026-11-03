![Stremio Origin](docs/header-rounded.png)

<p align="center">
  <img src="docs/logo-pill.png" width="200" alt="Stremio Origin">
</p>

<h1 align="center">Stremio Origin</h1>

<p align="center">A native desktop app for Stremio addons, written in C. This branch targets Linux (Arch / Omarchy).</p>

<p align="center"><em>Stremio™ is a trademark of its owner. Stremio Origin is independent and unofficial, and is not affiliated with, endorsed by or sponsored by Stremio. The name only describes compatibility.</em></p>

<p align="center">You bring your own Stremio account and your own addons. The app ships empty, hosts and indexes nothing, and plays only the stream links your addons return.</p>

## Features
- Home, Discover, Library and Search from your addons, with artwork, logos and trailers
- Playback through libmpv (mpv + FFmpeg): DTS, TrueHD and Dolby Vision play as-is, HDR tone-mapped
- The best stream for your connection, picked in one press
- Buttons and highlights take their colour from each title's logo or artwork
- People, reviews and a spotlight of what's new on every title
- A player with episodes, sources, subtitles and audio, skip intro and recap, and a next-episode card
- Found sources are fetched while a title plays, so the Sources list opens instantly
- Styled subtitles as the film made them, and subtitle sync
- Torrent playback with an optional local disk cache (512 MB / 1 GB / 2 GB, kept until another title plays) or a USB drive
- Mouse-wheel volume with an on-screen indicator; picture size defaults to fill the screen
- A resizable window that tiles and floats, with a fullscreen button by the clock (F11 works too)
- Physical keyboard typing in Search, full mouse support throughout
- Library, watched state and Continue Watching synced with your Stremio account
- 11 languages, including Arabic (right to left)

## Install
Build from source on Arch Linux / Omarchy. No package yet.

```bash
# dependencies: SDL2, OpenGL (mesa), libmpv, curl, openssl, cmake + make (engine only)
sudo pacman -S sdl2 mesa mpv curl openssl cmake make gcc
git clone https://github.com/JJDizz1L/Stremio-Origin
cd Stremio-Origin
tools/build.sh linux            # -> build/linux/stremioorigin
tools/build-engine.sh linux     # -> build/linux/libsbengine.so (torrent playback)
./build/linux/stremioorigin
```

Settings and cache live under `~/.local/share/StremioOrigin` and `~/.cache/StremioOrigin`. Only one instance at a time: two copies running together overwrite each other's settings.

## Privacy
- Sign-in goes directly to Stremio. Your account stays on this machine.
- No analytics. Nothing is sent to the developer.
- Logs stay on this machine. Sharing them is your choice: the app seals them and shows a short code.

## Support
- Issues: https://github.com/JJDizz1L/Stremio-Origin/issues

## Credits
- Upstream TV app this port is based on: [Stremio Origin for webOS](https://github.com/stremio-origin/webos) (GPL-3.0).
- Playback: [mpv](https://mpv.io) (GPL-2.0+), used as a system library.
- Interface sounds: [Interface Sounds](https://kenney.nl/assets/interface-sounds) by Kenney (CC0).
- The glass look: [liquid-glass-react](https://github.com/rdev/liquid-glass-react) by rdev; the app ships a modified version.
- Torrents: [Nuvio Engine](https://github.com/NuvioMedia/nuvio-engine) (GPL-3.0); the app ships a modified version.

## Licence
GPL-3.0, see [LICENSE](LICENSE). Third-party components and their licences: [third_party/licenses/README.txt](third_party/licenses/README.txt).
