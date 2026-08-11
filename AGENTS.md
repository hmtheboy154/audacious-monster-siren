# Audacious Monster Siren - Agent & Vibe Coding Guide

Welcome, AI Agent or Contributor! This document provides essential context, architectural rules, and coding conventions for the `audacious-monster-siren` project. Please read this thoroughly before generating code or modifying the project.

## 🎯 Project Overview
This project is an **Audacious Media Player plugin suite** that fetches, caches, and streams soundtracks from the official Hypergryph **Monster Siren (Arknights)** API (`https://monster-siren.hypergryph.com/api/`). It provides a seamless native experience through a custom `msr://` URI scheme and native dockable catalog interfaces for both Qt and GTK modes.

## 🏗️ Architecture & Modules
The suite is built using CMake and is strictly divided into decoupled modules to support Audacious's dual-UI (`-i qt` / `-i gtk`) ecosystem:

1. **`msr_backend` (`src/monster_siren.cpp` & `.h`)**
   - **Role**: A UI-agnostic C++ static library. Handles API fetching, URL routing (`msr_parse_url`), JSON deserialization (`Qt6::Core QJsonDocument`), and offline disk caching.
   - **Networking Rule**: NEVER use `QNetworkAccessManager` or `QEventLoop`. All network I/O must use Audacious's native Virtual File System (`VFSFile` / `<libaudcore/vfs.h>`) so the backend runs natively in pure GTK environments without a Qt event loop (`qApp`).

2. **`monster-siren-input.so` (`src/msr_input.cpp`)**
   - **Role**: An Audacious `InputPlugin`. It registers the `msr://` URI scheme, expands album tracklists, probes remote track lengths (`probe_remote_length`), and routes streams into Audacious's audio pipeline.
   - **Flag**: `0` (UI-Agnostic). It must load cleanly in both Qt and GTK modes.

3. **`monster-siren-ui.so` (`src/msr_ui.cpp`)**
   - **Role**: The Qt 6 Dockable Catalog Widget for `audacious -i qt`.
   - **Flag**: `PluginQtOnly`. Inherits `GeneralPlugin` and implements `get_qt_widget()`.

4. **`monster-siren-gtk.so` (`src/msr_ui_gtk.cpp`)**
   - **Role**: The GTK 3 Dockable Catalog Widget for `audacious -i gtk`.
   - **Flag**: `PluginGLibOnly`. Inherits `GeneralPlugin` and implements `get_gtk_widget()`.

---

## 🚦 Critical Rules & Constraints

### 1. Thread Safety & Asynchronous Execution
Audacious plugins share the main player event loop. **Never block the GUI thread** with network requests or image decoding.
- **Data Loading**: Launch network calls inside `std::thread` workers (`start_async_image_loader`, `fetch_and_populate_catalog`).
- **UI Updates**: Always marshal data back to the GUI thread before touching UI components:
  - **Qt**: Use `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
  - **GTK**: Use `g_idle_add()`.
- **Exit & Module Unload**: Audacious `dlclose`s plugin modules during teardown while workers may still be running. Launch every long-lived worker with `msr_run_background()` so the registry can join it in `msr_wait_for_background_threads()`, which every plugin's `cleanup()` must call. Keep fetches abortable: `msr_fetch_bytes()` reads in bounded chunks and checks the shutdown flag between reads so a slow CDN never delays exit.
- **Never** arm shutdown or abort logic from the `"playback stop"` hook: it fires on every stop and track change, not only at exit, and aborted network reads mid-playback produced "could not be decoded" errors for uncached albums.
- **Global State**: Protect global memory caches (`g_cached_albums`, `g_cached_images`) using `std::mutex g_cache_mutex;`.

### 2. URL Scheme & Routing
We use a custom virtual filesystem scheme:
- `msr://album/<cid>`: Represents a full album container. The input plugin expands this into individual song streams.
- `msr://song/<cid>`: Represents a single playable track stream.
- Always strip query parameters (e.g., `?token=...`) via `strchr` when determining file extensions for the offline cache (e.g., `.mp3`, `.flac`).

### 3. Dependencies
- **libaudcore / libaudgui**: Core Audacious APIs. Use `String`, `Index<T>`, `VFSFile`, `Tuple`, and `PreferencesWidget`.
- **Qt6::Core**: Used strictly for headless JSON parsing (`QJsonObject`), `QString`, and `QCryptographicHash`. (No `Qt6::Network`).
- **GTK 3**: Used exclusively in `msr_ui_gtk.cpp` (`GtkTreeView`, `GtkIconView`, `GdkPixbuf`).

### 4. GTK Catalog Window (`msr_ui_gtk.cpp`)
- **View switching**: The scrolled window holds exactly one child; every switch removes the old page and attaches a fresh `GtkTreeView` / `GtkIconView`. Both widgets cache row/layout metrics from their first allocation and never re-layout after a runtime renderer rebind, and `GtkStack` page bookkeeping can go stale. Do not reintroduce a stack or reuse page widgets.
- **Icon view sizing**: Constrain the grid with `gtk_widget_set_size_request(icon_view, 1, -1)` + `gtk_widget_set_hexpand(TRUE)` so items flow into rows that scroll vertically instead of a horizontal strip.
- **Per-UI defaults**: The Qt module is `PluginQtOnly` and never loads in GTK mode, so `init()` must register the shared `[monster-siren]` options with `aud_config_set_defaults("monster-siren", msr_gtk_defaults)`.
- **Live preference application**: `msr_gtk_prefs_apply()` re-creates a dock window lost after toggling the plugin-list entry by firing `hook_call("dock plugin enabled" / "dock plugin disabled", self)`; get the handle with `aud_plugin_by_header(&aud_plugin_instance)` + `aud_plugin_get_enabled()`.
- **Simple List thumbnails**: The list renderer binds `COL_ICON_SMALL` (32 px) in Simple List and `COL_ICON` in Album List/Grid. Equal-set both columns when images load.
- **Diagnostics**: Trace view switches and page attaches with `AUDWARN("MSR-GTK: ...")` markers.

### 5. Settings & Build Options
- **Preference ownership**: The disk caching options (`enable_cache`, `cache_dir`, `cache_catalog`, `cache_images`, `cache_audio`, `manual_sync`) are defined and defaulted (`msr_input_defaults`) in `msr_input.cpp` — the module that executes the caching. The UI modules must NOT redefine them; they keep only the view-mode keys (`enable_dedicated_window`, `simple_mode_art`, `default_view_mode`).
- **Build switches**: `MSR_BUILD_QT_UI` and `MSR_BUILD_GTK_UI` (AUTO/ON/OFF) in `CMakeLists.txt` gate the two catalog UI modules. AUTO requires both the matching interface plugin (`qtui`/`gtkui`) in the Audacious plugin dir and the dev libraries; `ON` forces the module and hard-errors when deps are missing; the backend and input plugin always build.

---

## 📝 Coding Style & Commits
- **Audacious C++ Conventions**: Follow the upstream Audacious style. Use C++17/20 features natively. Prefer `aud_get_bool` / `aud_set_int` for cross-UI preferences.
- **Git Commits (Linux Kernel Style)**: 
  - Write descriptive commit messages.
  - Limit the subject line to 50 characters.
  - Wrap the body text at **72 characters** (use a proper newline format instead of infinitely long lines).
  - Explain *why* a change was made, especially for thread-safety or architecture decisions.
