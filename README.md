# Audacious Monster Siren Plugin (`audacious-monster-siren`)

A feature-rich Audacious plugin suite that fetches, catalogs, and plays music directly from the [Monster Siren](https://monster-siren.hypergryph.com/music) website (the official Arknights original soundtrack catalog by Hypergryph).

## Requirements

- **Audacious 4.6.1 or above** (`libaudcore`, `libaudqt` headers and libraries)
- **Qt 6** (`Core`, `Network`, `Widgets`) or Qt 5
- **CMake 3.16+** and a C++20 compatible compiler

## Features & Architecture

This project builds two synergistic Audacious plugin modules that share a high-performance, cached C++ backend:

1. **Monster Siren Input Plugin (`monster-siren-input.so`)**
   - **URL Recognition**: Automatically recognizes URLs from `https://monster-siren.hypergryph.com` or custom `msr://` URIs when opened via Audacious' **Open URL** (`Ctrl+L`) or **Add URL** dialogs.
   - **Album Track Expansion**: When an Album URL (`msr://album/<cid>` or `https://monster-siren.hypergryph.com/info/<cid>`) is added, the plugin queries Monster Siren's API (`/api/album/<cid>/detail`), expands the single URL into separate track items in the playlist, and populates proper track metadata (Title, Artist, Track Number, and Album Art).
   - **Cover Art & Tag Probing**: Seamlessly supplies high-resolution album/single cover art directly to Audacious (`read_tag`).
   - **Direct Audio Streaming**: Resolves track CIDs to high-quality remote audio streams (`/api/song/<cid>` -> `sourceUrl` on Hypergryph CDN) and delegates streaming playback transparently.

2. **Monster Siren Catalog UI & Settings (`monster-siren-ui.so`)**
   - **Dedicated Dockable Window**: Adds a dockable panel ("Monster Siren Catalog") to Audacious' Qt Interface. Can be snapped into any dock space or detached into a standalone window.
   - **Searchable Album Catalog**: Indexes and displays the entire Monster Siren album catalog with cover thumbnails, album titles, and artists. Includes an instant search bar to filter albums by name or artist.
   - **One-Click Playback**: Clicking or activating any album in the catalog view immediately cleans the current queue, adds all tracks of the album, and starts playback.
   - **Customizable Settings**: Provides an integrated settings page in Audacious Preferences allowing users to enable/disable the dedicated catalog window on demand.

---

## How Custom `msr://` URLs Work

The plugin registers the custom URI scheme `msr://` with Audacious' Virtual File System (VFS) and input prober (`InputPlugin::with_schemes({"msr"})`). This allows concise, direct referencing of Monster Siren resources from playlists or the **Open URL** prompt without relying on web URLs.

### 1. Album URLs (`msr://album/<cid>`)
- **Format**: `msr://album/<cid>` (where `<cid>` is the Monster Siren Album ID, e.g., `msr://album/8929` or `msr://album/3807`).
- **Behavior**:
  - When Audacious probes `msr://album/<cid>`, the input plugin calls `/api/album/<cid>/detail`.
  - If the album has $N$ tracks, the plugin sets Audacious' subtune count to $N$.
  - Audacious automatically expands the entry into $N$ playlist tracks (`msr://album/<cid>?1`, `msr://album/<cid>?2`, ..., `msr://album/<cid>?N`).
  - For each track `?index`, `read_tag()` queries the $index$-th song from the album detail and fills in the precise track title, artist, track number, and album cover art.

### 2. Song URLs (`msr://song/<cid>`)
- **Format**: `msr://song/<cid>` (where `<cid>` is the Monster Siren Song ID, e.g., `msr://song/515437`).
- **Behavior**:
  - Probes `/api/song/<cid>` directly to obtain metadata (`name`, `artists`, `albumCid`, `mvCoverUrl`).
  - When played, fetches `sourceUrl` (`https://web.hycdn.cn/siren/audio/...`) and streams the audio buffer.

### 3. Web URL Interception (`https://monster-siren.hypergryph.com/...`)
- In addition to `msr://`, the plugin intercepts standard web URLs pasted into **Open URL**:
  - `https://monster-siren.hypergryph.com/music/info/<cid>` and `https://monster-siren.hypergryph.com/info/<cid>` $\rightarrow$ treated as `msr://album/<cid>`.
  - `https://monster-siren.hypergryph.com/music/<cid>` and `https://monster-siren.hypergryph.com/song/<cid>` $\rightarrow$ treated as `msr://song/<cid>`.

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
