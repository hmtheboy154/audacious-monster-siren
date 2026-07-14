# Audacious Monster Siren Plugin (`audacious-monster-siren`)

A flagship, feature-rich Audacious plugin suite that fetches, catalogs, caches, and plays music directly from the [Monster Siren](https://monster-siren.hypergryph.com/music) website (the official Arknights original soundtrack catalog by Hypergryph).

## Requirements

- **Audacious 4.6.1 or above** (`libaudcore`, `libaudqt` headers and libraries)
- **Qt 6** (`Core`, `Network`, `Widgets`) or Qt 5
- **CMake 3.16+** and a C++20 compatible compiler

---

## Features & Architecture

This project builds two synergistic Audacious plugin modules powered by a high-performance, multi-threaded, and offline-capable C++ backend (`libmsr_backend.a`):

### 1. Monster Siren Input Plugin (`monster-siren-input.so`)
- **URL & Scheme Recognition**: Automatically recognizes URLs from `https://monster-siren.hypergryph.com` or custom `msr://` URIs (`msr://album/<cid>` and `msr://song/<cid>`) when opened via Audacious' **Open URL** (`Ctrl+L`) or **Add URL** dialogs.
- **Album Track Expansion**: When an Album URL is added, the plugin queries `/api/album/<cid>/detail`, dynamically sets Audacious's subtune count to the exact number of tracks ($N$), and expands into separate playlist entries (`msr://album/<cid>?1` through `?N`).
- **Resilient VFS Playback & Full Timestamp Seeking**: Probes track headers via `probe_remote_length()` to populate precise durations (`Tuple::Length`), enabling full song timestamps (`0:00 / 3:42`) and accurate seekbar dragging (`fseek` / HTTP `Range: bytes=`) across both individual songs and album tracks. Always opens a pristine `VFSFile` handle from byte 0 before handing off to native decoders (`sndfile`, `mpg123`, `ffaudio`) to prevent seek state corruption.
- **Automatic HTTP 403 CDN Token Recovery**: Monster Siren CDN (`hycdn.cn`) stream URLs contain access tokens (`?token=...`) that expire after several hours (`HTTP 403 Forbidden`). During stream initialization (`play()`), the plugin automatically detects connection or decoder failures on expired tokens, invalidates stale caches, and refetches fresh API URLs on the fly without interrupting playback or requiring manual user intervention.
- **Embedded Lyrics & Dual-Mode Tag Scanning**: Populates synchronized `.lrc` lyrics (`Tuple::Lyrics`) right into Audacious's built-in **Lyrics** (`lyrics-qt` / `lyrics-gtk`) dockable window. Uses intelligent dual-mode `read_tag()` detection:
  - **Fast Scan Mode (`image == null`)**: When Audacious scans tracks to populate playlist table rows, duration and lyrics are read instantly from local cache (`0 ms`) without making blocking network requests.
  - **Playback Preparation Mode (`image != null`)**: Right before `setup_playback()` runs (`or on right-click -> View Song Info`), exact lyrics and duration are verified and populated, ensuring the dockable `Lyrics` window shows synchronized lines immediately without triggering slow online provider timeouts or UI freezes.

### 2. Monster Siren Catalog UI & Settings (`monster-siren-ui.so`)
- **Multi-Mode Dockable Catalog Window**: Adds a dedicated dockable panel ("Monster Siren Catalog") to Audacious's Qt Interface with asynchronous background loading (`std::thread` + `QMetaObject::invokeMethod`) for zero startup delay. Features three switchable view layouts (`View` button in header bar or Preferences):
  1. **Simple List (`QTreeView`)**: Compact tabular list showing Album Title and Artistes with RAM conservation options.
  2. **Album List (`QListView`)**: Vertical card list rendering cover art thumbnails (64x64+), bold titles, and artist subtitles.
  3. **Grid / Icon View (`QListView::IconMode`)**: Multi-column grid layout displaying centered cover art cards (150x190) with word-wrapped titles beneath.
- **Instant Search & One-Click Playback**: Real-time filtering (`QSortFilterProxyModel`) across Album Titles and Artistes. Double-clicking any album clears the active queue and starts playback instantly (`aud_drct_pl_open_list`).
- **Persistent Disk Caching & Indexing**: Comprehensive offline storage engine customizable under **Audacious Preferences $\rightarrow$ Plugins $\rightarrow$ General $\rightarrow$ Monster Siren Catalog**:
  - **Directory Selection**: Configurable cache directory with a native Browse dialog (`~/.cache/audacious/monster-siren` by default) supporting automatic `file://` URI prefix sanitization.
  - **Granular Checkboxes**: Individual toggles for `Cache Catalog Data`, `Cache Album & Song Metadata`, `Cache Cover Art Images`, `Cache Audio Files (.mp3/.flac/.wav)`, and `Cache Lyrics (.lrc)`.
  - **Background Audio Caching**: When `Cache Audio Files` is enabled, tracks being played over the network automatically buffer and save to local disk on a background thread (`msr_fetch_and_cache_audio_background`), allowing transparent `file://` offline playback on subsequent listens.
  - **Manual Sync Mode**: Run 100% offline from local disk caches without network queries until explicitly forcing a catalog refresh via the top header bar (`Refresh` button).

---

## How Custom `msr://` URLs Work

The plugin registers the custom URI scheme `msr://` with Audacious's Virtual File System (VFS) (`InputPlugin::with_schemes({"msr"})`). This allows concise, direct referencing of Monster Siren resources from playlists:

### 1. Album URLs (`msr://album/<cid>`)
- **Format**: `msr://album/<cid>` (where `<cid>` is the Monster Siren Album ID, e.g., `msr://album/8929` or `msr://album/3807`).
- **Behavior**:
  - When Audacious probes `msr://album/<cid>`, the input plugin queries `/api/album/<cid>/detail`.
  - If the album has $N$ tracks, the plugin sets Audacious's subtune count to $N$, generating $N$ playlist tracks (`msr://album/<cid>?1` through `?N`).
  - For each track index, `read_tag()` populates exact track metadata (Title, Artist, Track Number, Album Title, Duration, Lyrics, and Cover Art).

### 2. Song URLs (`msr://song/<cid>`)
- **Format**: `msr://song/<cid>` (where `<cid>` is the Monster Siren Song ID, e.g., `msr://song/515437`).
- **Behavior**:
  - Probes `/api/song/<cid>` directly to obtain metadata (`name`, `artists`, `albumCid`, `mvCoverUrl`, `lyricUrl`).
  - If the song has no standalone MV cover art, it automatically resolves parent `albumCid` cover art so single songs always display high-resolution album art when added.

### 3. Web URL Interception (`https://monster-siren.hypergryph.com/...`)
- In addition to `msr://`, the plugin intercepts standard web URLs pasted into **Open URL**:
  - `https://monster-siren.hypergryph.com/music/info/<cid>` and `https://monster-siren.hypergryph.com/info/<cid>` $\rightarrow$ treated as `msr://album/<cid>`.
  - `https://monster-siren.hypergryph.com/music/<cid>` and `https://monster-siren.hypergryph.com/song/<cid>` $\rightarrow$ treated as `msr://song/<cid>`.

---

## Diagnostic Logging & Troubleshooting

The plugin includes detailed console diagnostics (`AUDINFO`, `AUDDBG`, `AUDWARN`) for tracing VFS probing, tag reading modes (`SCAN/FAST` vs `PLAYBACK/FULL`), disk cache hits, and HTTP 403 CDN token recovery.

To run Audacious with full verbose logging enabled:
```bash
audacious -V
```

---

## Build & Installation

### Compiling from Source
```bash
# Configure the build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Compile the plugin modules
cmake --build build -j$(nproc)
```

### Installing to Audacious
```bash
# Install to the system Audacious plugin directory (may require sudo)
sudo cmake --install build
```
Or, if testing locally with a custom Audacious prefix:
```bash
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/path/to/audacious
cmake --build build -j$(nproc) && cmake --install build
```
