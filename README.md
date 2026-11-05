# Audacious Monster Siren Plugin (`audacious-monster-siren`)

A flagship, feature-rich Audacious plugin suite that fetches, catalogs, caches, and plays music directly from the [Monster Siren](https://monster-siren.hypergryph.com/music) website (the official Arknights original soundtrack catalog by Hypergryph).

## Requirements

- **Audacious 4.6.1 or above** (`libaudcore` headers and libraries)
- **libcurl** (`libcurl` headers and libraries for non-blocking HTTPS transfers)
- **Qt 6** (`Core` for JSON/cache utilities; optional `Widgets` for the Qt catalog UI)
- **GTK 3** (optional `libgtk-3` dev headers for the GTK3 catalog UI)
- **CMake 3.16+** and a C++20 compatible compiler

---

## Features & Architecture

This project builds three synergistic Audacious plugin modules powered by a high-performance, multi-threaded, and offline-capable C++ backend (`libmsr_backend.a`):

### 1. Monster Siren Input Plugin (`monster-siren-input.so`)
- **URL & Scheme Recognition**: Automatically recognizes URLs from `https://monster-siren.hypergryph.com` or custom `msr://` URIs (`msr://album/<cid>` and `msr://song/<cid>`) when opened via Audacious' **Open URL** (`Ctrl+L`) or **Add URL** dialogs.
- **Album Track Expansion**: When an Album URL is added, the plugin queries `/api/album/<cid>/detail`, dynamically sets Audacious's subtune count to the exact number of tracks ($N$), and expands into separate playlist entries (`msr://album/<cid>?1` through `?N`).
- **Resilient VFS Playback & Full Timestamp Seeking**: Probes WAV, FLAC, and MP3 stream headers via `probe_remote_length()` to populate precise durations (`Tuple::Length`), enabling full song timestamps (`0:00 / 3:42`) and accurate seekbar dragging (`fseek` / HTTP `Range: bytes=`) across both individual songs and album tracks. Always opens a pristine `VFSFile` handle from byte 0 before handing off to native decoders (`sndfile`, `mpg123`, `ffaudio`) to prevent seek state corruption.
- **Automatic HTTP 403 CDN Token Recovery**: Monster Siren CDN (`hycdn.cn`) stream URLs contain access tokens (`?token=...`) that expire after several hours (`HTTP 403 Forbidden`). During duration probing and stream initialization (`play()`), the plugin automatically detects connection or decoder failures on expired tokens, invalidates stale caches across modules, and refetches fresh API URLs on the fly without interrupting playback or requiring manual user intervention.
- **Embedded Lyrics & Dual-Mode Tag Scanning**: Populates synchronized `.lrc` lyrics (`Tuple::Lyrics`) right into Audacious's built-in **Lyrics** (`lyrics-qt` / `lyrics-gtk`) dockable window. Uses dual-mode `read_tag()` handling:
  - **Playlist Scan Mode (`image == null`)**: Populates track metadata, bounded header durations (cached in memory/disk after the first probe), and lyrics without downloading cover art images for every row.
  - **Playback Preparation Mode (`image != null`)**: Right before `setup_playback()` runs (or on right-click -> View Song Info), fetches high-resolution album/MV cover art alongside verified duration and lyrics.

### 2. Monster Siren Catalog UI & Settings

A native dockable catalog window is provided for both Audacious interfaces, sharing the same backend, settings, and view modes:

**Qt (`monster-siren-ui.so`, `PluginQtOnly`)**
- **Multi-Mode Dockable Catalog Window**: Adds a dedicated dockable panel ("Monster Siren Catalog") to Audacious's Qt Interface with asynchronous background loading (`std::thread` + `QMetaObject::invokeMethod`) for zero startup delay. Features three switchable view layouts (`View` button in header bar or Preferences):
  1. **Simple List (`QTreeView`)**: Compact tabular list showing Album Title and Artistes with RAM conservation options.
  2. **Album List (`QListView`)**: Vertical card list rendering cover art thumbnails (64x64+), bold titles, and artist subtitles.
  3. **Grid / Icon View (`QListView::IconMode`)**: Multi-column grid layout displaying centered cover art cards (150x190) with word-wrapped titles beneath.
- **Instant Search & One-Click Playback**: Real-time filtering (`QSortFilterProxyModel`) across Album Titles and Artistes. Double-clicking any album clears the active queue and starts playback instantly (`aud_drct_pl_open_list`).
- **Persistent Disk Caching & Indexing**: Comprehensive offline storage engine for catalog data, cover art, audio tracks, and lyrics, customizable under **Audacious Preferences $\rightarrow$ Plugins $\rightarrow$ Input $\rightarrow$ Monster Siren Input Plugin** (the input plugin owns these settings because it performs the caching during playback):
  - **Directory Selection**: Configurable cache directory with a native Browse dialog (`~/.cache/audacious/monster-siren` by default) supporting automatic `file://` URI prefix sanitization.
  - **Granular Checkboxes**: Individual toggles for `Cache Catalog Metadata Index`, `Cache Album Cover Art Images`, and `Cache Audio Tracks & Lyrics during playback`.
  - **Background Audio Caching**: When `Cache Audio Tracks & Lyrics during playback` is enabled, tracks being played over the network stream directly into atomic on-disk cache files on a background thread (`msr_fetch_and_cache_audio_background`), allowing transparent `file://` offline playback on subsequent listens.
  - **Manual Sync Mode**: Run 100% offline from local disk caches without network queries until explicitly forcing a catalog refresh via the top header bar (`Refresh` button).

**GTK 3 (`monster-siren-gtk.so`, `PluginGLibOnly`)**:
- **Native GTK3 Dockable Window**: The same catalog experience (Simple List / Album List / Grid views, instant search, refresh button, double-click playback) as a GTK3 General plugin dockable panel for `audacious -i gtk`, with asynchronous workers marshaled via `g_idle_add()`.
- **Live View & Settings Sync**: The in-window view combo and the plugin settings apply immediately to the open window; disk caching options live under the input plugin's setting section, while the shared `[monster-siren]` view-mode defaults are registered per UI, so both interfaces read and write identical options.
- **Simple List Thumbnails**: The `Show album art icons in Simple List mode` option renders 32px thumbnails in list rows, identical to Qt behavior.

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

#### UI Module Selection

By default every UI module the installed Audacious actually supports is built. Two tri-state switches control the dockable catalog windows (`AUTO` / `ON` / `OFF`, default `AUTO`):

| Option | Builds |
|--------|--------|
| `MSR_BUILD_QT_UI` | `monster-siren-ui` (Qt interface) |
| `MSR_BUILD_GTK_UI` | `monster-siren-gtk` (GTK3 interface) |

- **`AUTO`**: the module is built when the matching interface plugin (`qtui` / `gtkui`) ships in the installed Audacious plugin directory **and** the dev libraries are available; otherwise it is skipped with an explanatory configure message.
- **`ON`**: force the module; configuration fails if the dev libraries are missing.
- **`OFF`**: skip the module.

The backend library and the input plugin are always built. Examples:

```bash
# Qt UI + input only (e.g. an Audacious without a GTK interface installed)
cmake -B build -S . -DMSR_BUILD_GTK_UI=OFF

# GTK3 UI + input only
cmake -B build -S . -DMSR_BUILD_QT_UI=OFF -DMSR_BUILD_GTK_UI=ON -B build-gtk

# Input plugin only
cmake -B build -S . -DMSR_BUILD_QT_UI=OFF -DMSR_BUILD_GTK_UI=OFF
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

---

## Credits

Inspired by [MSRD](https://github.com/Meph1sto666/MSRD), a third-party downloader for the Monster Siren OST by Meph1sto666, which was referenced while implementing the Monster Siren API client.

---

## License

Released under the Apache License 2.0. See [`LICENSE`](LICENSE).

Note that the license applies to this project's code only; the Monster Siren catalog content (album names, cover art, audio tracks, and lyrics) is the property of [Hypergryph](https://www.hypergryph.com/).
