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

---

## 📝 Coding Style & Commits
- **Audacious C++ Conventions**: Follow the upstream Audacious style. Use C++17/20 features natively. Prefer `aud_get_bool` / `aud_set_int` for cross-UI preferences.
- **Git Commits (Linux Kernel Style)**: 
  - Write descriptive commit messages.
  - Limit the subject line to 50 characters.
  - Wrap the body text at **72 characters** (use a proper newline format instead of infinitely long lines).
  - Explain *why* a change was made, especially for thread-safety or architecture decisions.
