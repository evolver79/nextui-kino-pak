# Jellyfin in Kino: plan

Agreed 2026-11-19. Server: home LAN only, plain http (e.g. `http://192.168.x.x:8096`). Login with Quick Connect.
Libraries: films, series, and some 4K / HDR / 10-bit (those need the server to transcode). Downloads come later.

## Connect
- Find servers with Jellyfin's UDP discovery (broadcast "who is JellyfinServer?" to port 7359); typing an
  address with the d-pad is the fallback.
- Quick Connect: `POST /QuickConnect/Initiate` gives a code, shown big in Display L dots; the user approves it in
  the Jellyfin app or web UI; `GET /QuickConnect/Connect?secret=` until approved, then
  `POST /Users/AuthenticateWithQuickConnect` gives the access token. Token + server + user id are kept in the
  user data folder. Every request sends `Authorization: MediaBrowser Client="Kino", Device="TrimUI Brick", ...`.

## Browse
- Root of the browser: **SD card** and **Jellyfin** rows.
- Jellyfin root: Continue watching, Next up, then the user's libraries (`/UserViews`).
- **Films and series libraries: poster grid.** 5 columns × 2 rows, posters 176×264 (2:3), gap 16, filling
  content 96–640. Focus = the round-button ring (4 px card gap + 4 px paper). On the poster only state marks:
  dot progress along the bottom (started), accent dot (new episodes), slightly dimmed (seen). The focused item's
  title is in the header band (Title style left, label right: "2021 · 1H 52M" / "3 SEASONS").
  D-pad moves, L1/R1 a page, Y sort/filter (A–Z, recently added, unwatched).
- **Inside a series: like a local folder** (layout a). Several seasons = folder rows; one season = straight to
  the episodes. Episode rows with the episode image in the detail card: title, "S1 · E3 · 44 MIN", overview,
  resume time and dot progress. A play/resume, X from start.
- Images: `/Items/{id}/Images/Primary?fillWidth=176&fillHeight=264&quality=85` (posters),
  `.../Images/Backdrop` or the episode's Primary at 428×240 for the detail card. JPEG decoded with FFmpeg's
  mjpeg decoder (already in the build), cached on the SD card.
- Same grid for local folders with `poster.jpg` / `folder.jpg` (later).

## Play
- Direct play when the Brick can: H.264 / HEVC 8-bit up to 1080p (the video engine), common audio. mpv opens
  `/Videos/{id}/stream?static=true&api_key=...`.
- Otherwise ask the server to transcode (10-bit, HDR, 4K, AV1): `/Items/{id}/PlaybackInfo` with a device profile
  (H.264 ≤1080p, AAC/AC3 stereo, max bitrate ~12 Mbit/s), play the HLS url it returns.
- Report progress: `/Sessions/Playing` on start, `/Sessions/Playing/Progress` every 10 s and on pause/seek,
  `/Sessions/Playing/Stopped` on stop. Resume and seen come from the server.
- External subtitles: `sub-add` the server's subtitle stream urls.

## Build
- FFmpeg is built with `--disable-network`: turn networking on (http/https/hls), https through the Brick's
  OpenSSL (dynamic, like Musicdisk). API calls with Musicdisk's small `http.c`, plus POST.

## Phases
1. Discovery, Quick Connect, libraries, poster grid, series as folders, direct play, progress reporting.
2. Continue watching / Next up, transcoding, external subtitles.
3. Later: downloads for offline, audio track choice per file, several users.
