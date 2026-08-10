/**
 * monster_siren.cpp - Shared Backend Library (`libmsr_backend`)
 *
 * Implements communication with the official Hypergryph Monster Siren API
 * (`https://monster-siren.hypergryph.com/api/...`). Provides:
 * 1. Resilient HTTP/HTTPS request execution over Audacious's VFS transport with automatic retry + backoff.
 * 2. JSON deserialization of albums, songs, and artists.
 * 3. Thread-safe in-memory caching (`std::mutex`) for album details, song metadata, and cover art images.
 * 4. URL parsing (`msr_parse_url`) converting custom `msr://` and web URLs into structured routing entities.
 * 5. Background thread registry so plugins can join their workers at shutdown.
 */

#include <libaudcore/vfs.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QThread>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <mutex>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdlib>

#include <libaudcore/runtime.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/plugin.h>
#include <QStandardPaths>

#include "monster_siren.h"

// Global thread-safe cache to prevent redundant API calls during playlist traversal & UI rendering
static std::mutex g_cache_mutex;
static std::vector<MsrAlbum> g_cached_albums;
static bool g_albums_cached = false;
static std::map<std::string, MsrAlbum> g_cached_album_details;
static std::map<std::string, MsrSongDetail> g_cached_song_details;
static std::map<std::string, Index<char>> g_cached_images;
static std::map<std::string, String> g_cached_lyrics;

/* Every module (input + each UI plugin) gets its own copy of this registry
 * because the backend is a static library; each plugin only ever joins the
 * threads it spawned itself, which is exactly what its cleanup() needs. */
static std::mutex g_thread_mutex;
static std::vector<std::thread> g_threads;
static std::atomic<bool> g_shutting_down{false};

void msr_run_background(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    g_threads.emplace_back(std::move(task));
}

void msr_begin_shutdown()
{
    g_shutting_down = true;
}

void msr_wait_for_background_threads()
{
    msr_begin_shutdown();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        threads.swap(g_threads);
    }

    for (auto & thread : threads)
        if (thread.joinable())
            thread.join();
}

/**
 * Executes a GET request over Audacious's VFS transport and returns raw
 * response bytes. Features a 3-attempt retry loop with linear backoff to
 * gracefully handle socket/SSL handshakes or temporary network contention
 * (especially during concurrent Audacious startup). The body is read in
 * bounded chunks so shutdown can abort the transfer promptly instead of
 * blocking process exit on a full-track (or hung) download.
 */
static QByteArray msr_fetch_bytes(const QString & url)
{
    String aud_url(url.toUtf8().constData());
    for (int attempt = 0; attempt < 3; attempt++) {
        if (g_shutting_down)
            return QByteArray();

        VFSFile file(aud_url, "r");
        if (file) {
            QByteArray out;
            char buf[65536];
            while (!g_shutting_down) {
                int64_t n = file.fread(buf, 1, sizeof(buf));
                if (n <= 0)
                    break;
                out.append(buf, (int)n);
            }
            if (g_shutting_down)
                return QByteArray();
            if (out.size() > 0)
                return out;
        }

        if (attempt < 2 && !g_shutting_down)
            QThread::msleep(150 * (attempt + 1));
    }
    return QByteArray();
}

static QJsonDocument msr_fetch_json(const QString & url)
{
    QByteArray bytes = msr_fetch_bytes(url);
    if (bytes.isEmpty())
        return QJsonDocument();
    return QJsonDocument::fromJson(bytes);
}

MsrUrlParsed msr_parse_url(const char * url)
{
    MsrUrlParsed result;
    if (!url || !url[0])
        return result;

    std::string s(url);

    // Extract a numeric subtune index from a trailing ?N query segment
    // (e.g. msr://album/8929?1). Any other query string (e.g. ?lang=cn) or
    // fragment is dropped so it cannot leak into the parsed cid.
    int subtune = 0;
    size_t qpos = s.rfind('?');
    if (qpos != std::string::npos) {
        std::string sub_str = s.substr(qpos + 1);
        char * endp = nullptr;
        long val = std::strtol(sub_str.c_str(), &endp, 10);
        if (endp && *endp == '\0' && val > 0)
            subtune = (int)val;
        s = s.substr(0, qpos);
    }

    size_t hashpos = s.find('#');
    if (hashpos != std::string::npos)
        s = s.substr(0, hashpos);

    // Strip trailing slashes
    while (!s.empty() && s.back() == '/')
        s.pop_back();

    // Extracts the cid starting at <start>; cuts it at the next path segment
    auto cid_at = [](const std::string & str, size_t start) {
        std::string cid = str.substr(start);
        size_t slash = cid.find('/');
        if (slash != std::string::npos)
            cid.resize(slash);
        return cid;
    };

    // Check custom scheme msr://
    if (s.rfind("msr://album/", 0) == 0) {
        result.cid = String(cid_at(s, 12).c_str());
        result.subtune_index = subtune;
        result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
        return result;
    }
    if (s.rfind("msr://song/", 0) == 0) {
        result.cid = String(cid_at(s, 11).c_str());
        result.type = MsrUrlType::Song;
        return result;
    }

    // Check web URLs from monster-siren.hypergryph.com
    size_t domain_pos = s.find("monster-siren.hypergryph.com");
    if (domain_pos != std::string::npos) {
        const char * marker = nullptr;
        size_t start = 0;
        bool album = false;

        // /api/album/<cid>[/detail]
        marker = "/api/album/";
        start = s.find(marker);
        if (start != std::string::npos) {
            album = true;
        } else {
            // /api/song/<cid>
            marker = "/api/song/";
            start = s.find(marker);
            if (start == std::string::npos) {
                // /music/info/<cid> (album page) must win over generic /music/<cid>
                marker = "/music/info/";
                start = s.find(marker);
                if (start != std::string::npos)
                    album = true;
                else {
                    // /info/<cid> (album page)
                    marker = "/info/";
                    start = s.find(marker);
                    if (start != std::string::npos)
                        album = true;
                    else {
                        // /music/<cid> (song page)
                        marker = "/music/";
                        start = s.find(marker);
                        if (start == std::string::npos) {
                            // /song/<cid>
                            marker = "/song/";
                            start = s.find(marker);
                        }
                    }
                }
            }
        }

        if (start != std::string::npos) {
            result.subtune_index = subtune;
            if (album) {
                result.cid = String(cid_at(s, start + strlen(marker)).c_str());
                result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
            } else {
                result.cid = String(cid_at(s, start + strlen(marker)).c_str());
                result.type = MsrUrlType::Song;
            }
            return result;
        }
    }

    return result;
}

String msr_get_cache_dir()
{
    String dir = aud_get_str("monster-siren", "cache_dir");
    if (!dir || !dir[0]) {
        QString qcache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
        if (qcache.isEmpty())
            qcache = QDir::homePath() + "/.cache";
        QString qpath = QDir(qcache).filePath("audacious/monster-siren");
        StringBuf def = str_copy(qpath.toUtf8().constData());
        aud_set_str("monster-siren", "cache_dir", def);
        dir = String(def);
    } else {
        QString qstr = QString::fromUtf8((const char *)dir);
        if (qstr.startsWith("file://")) {
            qstr = QUrl(qstr).toLocalFile();
            if (qstr.isEmpty())
                qstr = QString::fromUtf8((const char *)dir).mid(7);
            StringBuf cleaned = str_copy(qstr.toUtf8().constData());
            aud_set_str("monster-siren", "cache_dir", cleaned);
            dir = String(cleaned);
        }
    }
    return dir;
}

static QString msr_url_to_hash(const char * s)
{
    if (!s) return QString();
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray(s), QCryptographicHash::Md5).toHex());
}

bool msr_load_catalog_from_cache(std::vector<MsrAlbum> & out_albums)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog"))
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf filepath = filename_build({cache_dir, "catalog.json"});
    QFile file((const char *)filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonArray list = doc.object().value("data").toArray();
    if (list.isEmpty())
        return false;

    std::vector<MsrAlbum> albums;
    for (const QJsonValue & val : list) {
        QJsonObject obj = val.toObject();
        MsrAlbum album;
        album.cid = String(obj.value("cid").toString().toUtf8().constData());
        album.name = String(obj.value("name").toString().toUtf8().constData());
        album.coverUrl = String(obj.value("coverUrl").toString().toUtf8().constData());
        album.artistes = String(obj.value("artistes").toString().toUtf8().constData());
        albums.push_back(std::move(album));
    }
    out_albums = std::move(albums);
    return true;
}

void msr_save_catalog_to_cache(const std::vector<MsrAlbum> & albums)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog") || albums.empty())
        return;

    String cache_dir = msr_get_cache_dir();
    QDir().mkpath(QString::fromUtf8((const char *)cache_dir));

    StringBuf filepath = filename_build({cache_dir, "catalog.json"});
    QJsonArray arr;
    for (const auto & alb : albums) {
        QJsonObject obj;
        obj["cid"] = (const char *)alb.cid;
        obj["name"] = (const char *)alb.name;
        obj["coverUrl"] = (const char *)alb.coverUrl;
        obj["artistes"] = (const char *)alb.artistes;
        arr.append(obj);
    }
    QJsonObject root;
    root["data"] = arr;
    QJsonDocument doc(root);
    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write(doc.toJson(QJsonDocument::Compact));
}

bool msr_find_cached_art(const String & cid_or_url, String & out_path)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_images") || !cid_or_url)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf art_dir = filename_build({cache_dir, "art"});
    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({art_dir, StringBuf(str_concat({"cover_", hash.toUtf8().constData(), ".jpg"}))});
    if (QFile::exists((const char *)filepath)) {
        out_path = String(filepath);
        return true;
    }
    return false;
}

void msr_save_art_to_cache(const String & cid_or_url, const Index<char> & data)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_images") || !cid_or_url || !data.len())
        return;

    String cache_dir = msr_get_cache_dir();
    StringBuf art_dir = filename_build({cache_dir, "art"});
    QDir().mkpath(QString::fromUtf8((const char *)art_dir));

    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({art_dir, StringBuf(str_concat({"cover_", hash.toUtf8().constData(), ".jpg"}))});
    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write(data.begin(), data.len());
}

bool msr_find_cached_audio(const String & cid, String & out_path)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio") || !cid)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf audio_dir = filename_build({cache_dir, "audio"});

    const char * exts[] = {".mp3", ".flac", ".wav", ".aac", ".m4a", ".ogg", ".wma"};
    for (const char * ext : exts) {
        StringBuf filepath = filename_build({audio_dir, StringBuf(str_concat({"audio_", cid, ext}))});
        if (QFile::exists((const char *)filepath)) {
            out_path = String(filepath);
            return true;
        }
    }
    return false;
}

void msr_save_audio_to_cache(const String & cid, const String & sourceUrl, const Index<char> & data)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio") || !cid || !sourceUrl || !data.len())
        return;

    String cache_dir = msr_get_cache_dir();
    StringBuf audio_dir = filename_build({cache_dir, "audio"});
    QDir().mkpath(QString::fromUtf8((const char *)audio_dir));

    // Strip query parameters before finding extension (CDN URLs contain ?token=...)
    std::string url_clean((const char *)sourceUrl);
    size_t qmark = url_clean.find('?');
    if (qmark != std::string::npos)
        url_clean = url_clean.substr(0, qmark);
    const char * dot = strrchr(url_clean.c_str(), '.');
    String ext = (dot && strlen(dot) <= 5) ? String(dot) : String(".mp3");
    StringBuf filepath = filename_build({audio_dir, StringBuf(str_concat({"audio_", cid, ext}))});
    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write(data.begin(), data.len());
}

bool msr_find_cached_lyrics(const String & cid_or_url, String & out_path)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio") || !cid_or_url)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf lyrics_dir = filename_build({cache_dir, "lyrics"});
    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({lyrics_dir, StringBuf(str_concat({"lyric_", hash.toUtf8().constData(), ".lrc"}))});
    if (QFile::exists((const char *)filepath)) {
        out_path = String(filepath);
        return true;
    }
    return false;
}

void msr_save_lyrics_to_cache(const String & cid_or_url, const String & lyrics_text)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio") || !cid_or_url || !lyrics_text)
        return;

    String cache_dir = msr_get_cache_dir();
    StringBuf lyrics_dir = filename_build({cache_dir, "lyrics"});
    QDir().mkpath(QString::fromUtf8((const char *)lyrics_dir));

    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({lyrics_dir, StringBuf(str_concat({"lyric_", hash.toUtf8().constData(), ".lrc"}))});
    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write((const char *)lyrics_text, strlen((const char *)lyrics_text));
}

void msr_fetch_and_cache_audio_background(const String & cid, const String & sourceUrl)
{
    if (!cid || !sourceUrl || !aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio"))
        return;
    String path;
    if (msr_find_cached_audio(cid, path))
        return;
    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)sourceUrl));
    if (!bytes.isEmpty()) {
        Index<char> data;
        data.insert(bytes.constData(), 0, bytes.size());
        msr_save_audio_to_cache(cid, sourceUrl, data);
    }
}

std::vector<MsrAlbum> msr_fetch_albums(bool force_network)
{
    if (!force_network) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_albums_cached && !g_cached_albums.empty())
            return g_cached_albums;
    }

    if (!force_network && aud_get_bool("monster-siren", "enable_cache") &&
        aud_get_bool("monster-siren", "cache_catalog") &&
        aud_get_bool("monster-siren", "manual_sync"))
    {
        std::vector<MsrAlbum> disk_albums;
        if (msr_load_catalog_from_cache(disk_albums)) {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            g_cached_albums = disk_albums;
            g_albums_cached = true;
            return g_cached_albums;
        }
    }

    QJsonDocument doc = msr_fetch_json("https://monster-siren.hypergryph.com/api/albums");
    std::vector<MsrAlbum> albums;
    if (!doc.isObject()) {
        if (!force_network && msr_load_catalog_from_cache(albums)) {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            g_cached_albums = albums;
            g_albums_cached = true;
            return g_cached_albums;
        }
        return albums;
    }

    QJsonObject root = doc.object();
    QJsonArray list = root.value("data").toArray();

    for (const QJsonValue & val : list) {
        QJsonObject obj = val.toObject();
        MsrAlbum album;
        album.cid = String(obj.value("cid").toString().toUtf8().constData());
        album.name = String(obj.value("name").toString().toUtf8().constData());
        album.coverUrl = String(obj.value("coverUrl").toString().toUtf8().constData());
        
        QJsonArray art_arr = obj.value("artistes").toArray();
        QStringList art_list;
        for (const QJsonValue & a : art_arr)
            art_list.append(a.toString());
        album.artistes = String(art_list.join(", ").toUtf8().constData());

        albums.push_back(std::move(album));
    }

    if (!albums.empty())
        msr_save_catalog_to_cache(albums);

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_albums = albums;
        g_albums_cached = true;
    }

    return albums;
}

bool msr_find_cached_album_detail(const String & cid, MsrAlbum & out_album)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog") || !cid)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    StringBuf filepath = filename_build({meta_dir, StringBuf(str_concat({"album_", (const char *)cid, ".json"}))});
    QFile file((const char *)filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject obj = doc.object();
    out_album.cid = String(obj.value("cid").toString().toUtf8().constData());
    out_album.name = String(obj.value("name").toString().toUtf8().constData());
    out_album.coverUrl = String(obj.value("coverUrl").toString().toUtf8().constData());
    out_album.artistes = String(obj.value("artistes").toString().toUtf8().constData());

    QJsonArray tracks_arr = obj.value("tracks").toArray();
    for (const QJsonValue & tv : tracks_arr) {
        QJsonObject tobj = tv.toObject();
        MsrTrack track;
        track.cid = String(tobj.value("cid").toString().toUtf8().constData());
        track.name = String(tobj.value("name").toString().toUtf8().constData());
        track.artistes = String(tobj.value("artistes").toString().toUtf8().constData());
        out_album.tracks.push_back(std::move(track));
    }
    return true;
}

void msr_save_album_detail_to_cache(const MsrAlbum & album)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog") || !album.cid)
        return;

    String cache_dir = msr_get_cache_dir();
    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    QDir().mkpath(QString::fromUtf8((const char *)meta_dir));

    StringBuf filepath = filename_build({meta_dir, StringBuf(str_concat({"album_", (const char *)album.cid, ".json"}))});
    QJsonObject obj;
    obj["cid"] = (const char *)album.cid;
    obj["name"] = (const char *)album.name;
    obj["coverUrl"] = (const char *)album.coverUrl;
    obj["artistes"] = (const char *)album.artistes;

    QJsonArray tracks_arr;
    for (const auto & trk : album.tracks) {
        QJsonObject tobj;
        tobj["cid"] = (const char *)trk.cid;
        tobj["name"] = (const char *)trk.name;
        tobj["artistes"] = (const char *)trk.artistes;
        tracks_arr.append(tobj);
    }
    obj["tracks"] = tracks_arr;

    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool msr_find_cached_song_detail(const String & cid, MsrSongDetail & out_song)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog") || !cid)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    StringBuf filepath = filename_build({meta_dir, StringBuf(str_concat({"song_", (const char *)cid, ".json"}))});
    QFile file((const char *)filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject obj = doc.object();
    out_song.cid = String(obj.value("cid").toString().toUtf8().constData());
    out_song.name = String(obj.value("name").toString().toUtf8().constData());
    out_song.albumCid = String(obj.value("albumCid").toString().toUtf8().constData());
    out_song.sourceUrl = String(obj.value("sourceUrl").toString().toUtf8().constData());
    out_song.lyricUrl = String(obj.value("lyricUrl").toString().toUtf8().constData());
    out_song.mvCoverUrl = String(obj.value("mvCoverUrl").toString().toUtf8().constData());
    out_song.artists = String(obj.value("artists").toString().toUtf8().constData());
    out_song.duration_ms = obj.value("duration_ms").toInt(0);
    return true;
}

void msr_save_song_detail_to_cache(const MsrSongDetail & song)
{
    /* Always refresh the in-memory cache, even when disk caching is disabled.
     * This ensures that durations learned via stream probing stay available
     * for the whole session (e.g. so the playlist tuple carries Tuple::Length
     * and seeking works). */
    if (!song.cid)
        return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_song_details[std::string((const char *)song.cid)] = song;
    }

    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_catalog"))
        return;

    String cache_dir = msr_get_cache_dir();
    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    QDir().mkpath(QString::fromUtf8((const char *)meta_dir));

    StringBuf filepath = filename_build({meta_dir, StringBuf(str_concat({"song_", (const char *)song.cid, ".json"}))});
    QJsonObject obj;
    obj["cid"] = (const char *)song.cid;
    obj["name"] = (const char *)song.name;
    obj["albumCid"] = (const char *)song.albumCid;
    obj["sourceUrl"] = (const char *)song.sourceUrl;
    obj["lyricUrl"] = (const char *)song.lyricUrl;
    obj["mvCoverUrl"] = (const char *)song.mvCoverUrl;
    obj["artists"] = (const char *)song.artists;
    obj["duration_ms"] = song.duration_ms;

    QFile file((const char *)filepath);
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void msr_invalidate_song_detail(const String & cid)
{
    if (!cid) return;
    std::string key((const char *)cid);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_song_details.erase(key);
    }
    String cache_dir = msr_get_cache_dir();
    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    StringBuf filepath = filename_build({meta_dir, StringBuf(str_concat({"song_", (const char *)cid, ".json"}))});
    QFile::remove((const char *)filepath);
}

MsrAlbum msr_fetch_album_detail(const String & cid)
{
    if (!cid)
        return MsrAlbum();

    std::string key((const char *)cid);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cached_album_details.find(key);
        if (it != g_cached_album_details.end())
            return it->second;
    }

    MsrAlbum disk_album;
    if (msr_find_cached_album_detail(cid, disk_album)) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_album_details[key] = disk_album;
        return disk_album;
    }

    QString url = QString("https://monster-siren.hypergryph.com/api/album/%1/detail").arg((const char *)cid);
    QJsonDocument doc = msr_fetch_json(url);
    if (!doc.isObject())
        return MsrAlbum();

    QJsonObject data = doc.object().value("data").toObject();
    MsrAlbum album;
    album.cid = String(data.value("cid").toString().toUtf8().constData());
    album.name = String(data.value("name").toString().toUtf8().constData());
    album.coverUrl = String(data.value("coverUrl").toString().toUtf8().constData());

    QJsonArray art_arr = data.value("artistes").toArray();
    QStringList art_list;
    for (const QJsonValue & a : art_arr)
        art_list.append(a.toString());
    album.artistes = String(art_list.join(", ").toUtf8().constData());

    QJsonArray songs_arr = data.value("songs").toArray();
    for (const QJsonValue & sval : songs_arr) {
        QJsonObject sobj = sval.toObject();
        MsrTrack track;
        track.cid = String(sobj.value("cid").toString().toUtf8().constData());
        track.name = String(sobj.value("name").toString().toUtf8().constData());
        
        QJsonArray tart_arr = sobj.value("artistes").toArray();
        QStringList tart_list;
        for (const QJsonValue & ta : tart_arr)
            tart_list.append(ta.toString());
        track.artistes = String(tart_list.join(", ").toUtf8().constData());

        album.tracks.push_back(std::move(track));
    }

    msr_save_album_detail_to_cache(album);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_album_details[key] = album;
    }

    return album;
}

MsrSongDetail msr_fetch_song_detail(const String & cid)
{
    if (!cid)
        return MsrSongDetail();

    std::string key((const char *)cid);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cached_song_details.find(key);
        if (it != g_cached_song_details.end())
            return it->second;
    }

    MsrSongDetail disk_song;
    if (msr_find_cached_song_detail(cid, disk_song)) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_song_details[key] = disk_song;
        return disk_song;
    }

    QString url = QString("https://monster-siren.hypergryph.com/api/song/%1").arg((const char *)cid);
    QJsonDocument doc = msr_fetch_json(url);
    if (!doc.isObject())
        return MsrSongDetail();

    QJsonObject data = doc.object().value("data").toObject();
    MsrSongDetail detail;
    detail.cid = String(data.value("cid").toString().toUtf8().constData());
    detail.name = String(data.value("name").toString().toUtf8().constData());
    detail.albumCid = String(data.value("albumCid").toString().toUtf8().constData());
    detail.sourceUrl = String(data.value("sourceUrl").toString().toUtf8().constData());
    detail.lyricUrl = String(data.value("lyricUrl").toString().toUtf8().constData());
    detail.mvCoverUrl = String(data.value("mvCoverUrl").toString().toUtf8().constData());

    QJsonArray art_arr = data.value("artists").toArray();
    QStringList art_list;
    for (const QJsonValue & a : art_arr)
        art_list.append(a.toString());
    detail.artists = String(art_list.join(", ").toUtf8().constData());

    msr_save_song_detail_to_cache(detail);
    return detail;
}

Index<char> msr_fetch_image(const String & url)
{
    Index<char> result;
    if (!url || !url[0])
        return result;

    std::string key((const char *)url);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cached_images.find(key);
        if (it != g_cached_images.end()) {
            Index<char> copy;
            copy.insert(it->second.begin(), 0, it->second.len());
            return copy;
        }
    }

    String cached_path;
    if (msr_find_cached_art(url, cached_path)) {
        QFile file((const char *)cached_path);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray bytes = file.readAll();
            if (!bytes.isEmpty()) {
                result.insert(bytes.constData(), 0, bytes.size());
                return result; // Return directly from disk cache without permanently holding inside g_cached_images RAM map
            }
        }
    }

    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)url));
    if (!bytes.isEmpty()) {
        result.insert(bytes.constData(), 0, bytes.size());
        msr_save_art_to_cache(url, result);
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_cached_images.size() >= 10)
            g_cached_images.clear(); // Keep RAM memory footprint of g_cached_images strictly capped at <= 10 covers
        Index<char> cache_copy;
        cache_copy.insert(result.begin(), 0, result.len());
        g_cached_images[key] = std::move(cache_copy);
    }

    return result;
}

String msr_fetch_lyrics(const String & url)
{
    if (!url || !url[0])
        return String();

    std::string key((const char *)url);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cached_lyrics.find(key);
        if (it != g_cached_lyrics.end())
            return it->second;
    }

    String cached_path;
    if (msr_find_cached_lyrics(url, cached_path)) {
        AUDINFO("MSR: msr_fetch_lyrics cache hit for %s -> %s\n", (const char *)url, (const char *)cached_path);
        QFile file((const char *)cached_path);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray bytes = file.readAll();
            if (!bytes.isEmpty()) {
                String result(bytes.constData());
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                g_cached_lyrics[key] = result;
                return result;
            }
        }
    }

    AUDINFO("MSR: msr_fetch_lyrics downloading over HTTPS: %s\n", (const char *)url);
    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)url));
    if (bytes.isEmpty()) {
        AUDWARN("MSR: msr_fetch_lyrics failed or empty response for %s\n", (const char *)url);
        return String();
    }

    String result(bytes.constData());
    msr_save_lyrics_to_cache(url, result);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_lyrics[key] = result;
    }
    return result;
}

void msr_cache_clear()
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cached_albums.clear();
    g_albums_cached = false;
    g_cached_album_details.clear();
    g_cached_song_details.clear();
    g_cached_images.clear();
    g_cached_lyrics.clear();
}

void msr_invalidate_catalog_cache()
{
    /* Remove the on-disk catalog index plus all cached album/song detail
     * JSON. The cached cover art, audio files and lyrics are intentionally
     * kept: they stay valid and refresh only needs the metadata to be
     * re-fetched. */
    String cache_dir = msr_get_cache_dir();

    StringBuf meta_dir = filename_build({cache_dir, "meta"});
    QDir meta((const char *)meta_dir);
    meta.removeRecursively();

    StringBuf filepath = filename_build({cache_dir, "catalog.json"});
    QFile::remove((const char *)filepath);

    msr_cache_clear();
}
