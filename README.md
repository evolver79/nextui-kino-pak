# Kino – video player for NextUI

Plays films and series on the TrimUI Brick (`tg5040`), from the SD card's `Videos` folder or from a
[Jellyfin](https://jellyfin.org) server on the home network. Drawn with [Dot](https://github.com/evolver79/nextui-dotui),
the shared look of Kino, Musicdisk and Tuner.

- **Hardware decoding:** H.264 and 8-bit HEVC up to 1080p play on the Brick's video engine (Allwinner CedarC, through
  an FFmpeg decoder in `src/cedar`), about one CPU core at 24 fps. Everything else decodes in software.
- **SD card:** folders and files as rows with a picture and the facts of the selected one (length, resolution, codec).
  A folder of films shows as covers instead: a picture next to the film (`Film.jpg`, `Film-poster.jpg`, or
  `poster.jpg` / `folder.jpg` in a film's folder), otherwise the best of four frames from the film.
- **Resume:** Kino remembers where you stopped. A resumes, X plays from the start; nearly finished counts as seen.
- **Jellyfin:** finds the server on the network and logs in with Quick Connect (a code you approve in the Jellyfin
  app). Continue watching and Next up first, films and series as a cover grid, seasons and episodes as rows. Plays
  directly when the Brick can, otherwise the server converts it. Progress is reported back to the server.
- **While playing:** picture filters (sharpen, warm, cool, black & white, retro CRT), aspect (original, 4:3, 16:9,
  stretch, zoom), brightness, contrast, deinterlacing, subtitles with a preferred language, and the Brick's lights
  can follow the colour of the picture.

## Buttons

| | Browsing | Playing |
|---|---|---|
| A | open, play, resume | pause |
| B | back (quit at the top) | stop and save the place |
| X | play from the start | |
| Y | options | options |
| ◄ ► | a page (rows) or move (covers) | 10 seconds back, forward |
| ▼ | | to the progress bar to scrub; A jumps there |
| L1 / R1 | a page | picture filter |
| L2 / R2 | | aspect |
| SELECT | sort the covers: name, recently added, unwatched | subtitles on/off |
| MENU | quit | quit (the place is saved) |

In Options, X shows the buttons screen (it also opens on the first start).

## Install

Copy `Kino.pak` into `Tools/tg5040/` on the SD card and put videos in `Videos/`. Settings, resume positions and the
picture cache are kept in NextUI's user data folder (`.userdata/tg5040/Kino`).

## Build

Needs Docker (an ARM64 Debian bullseye container). `git clone --recursive` (Dot is a submodule in `ui/dot`), then:

```sh
build/build.sh    # fetches the library sources, builds FFmpeg, libass and libmpv once (several minutes), then build/out/kino
build/shots.sh    # renders the screens headless to build/out/shots/*.png
```

Copy `build/out/kino` and `ui/dot/fonts/` into `Kino.pak/`. (`deploy.sh` does that and copies the pak to the Brick
with a small private helper.)

## Licence

MIT. FFmpeg and mpv are LGPL/GPL, libass, FreeType, FriBidi and HarfBuzz have their own free licences, cJSON is MIT,
and the fonts (Doto, Geist) are SIL OFL.
