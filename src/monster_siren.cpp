/**
 * monster_siren.cpp - Shared Backend Library (`libmsr_backend`)
 *
 * Implements communication with the official Hypergryph Monster Siren API
 * (`https://monster-siren.hypergryph.com/api/...`). Provides:
 * 1. Resilient HTTP/HTTPS request execution over Qt (`QNetworkAccessManager`) with automatic retry + backoff.
 * 2. JSON deserialization of albums, songs, and artists.
 * 3. Thread-safe in-memory caching (`std::mutex`) for album details, song metadata, and cover art images.
 * 4. URL parsing (`msr_parse_url`) converting custom `msr://` and web URLs into structured routing entities.
 */

#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QThread>
#include <mutex>
#include <map>
#include <cstring>
#include <cstdlib>

#include "monster_siren.h"

// Global thread-safe cache to prevent redundant API calls during playlist traversal & UI rendering
static std::mutex g_cache_mutex;
static std::vector<MsrAlbum> g_cached_albums;
static bool g_albums_cached = false;
static std::map<std::string, MsrAlbum> g_cached_album_details;
static std::map<std::string, MsrSongDetail> g_cached_song_details;
static std::map<std::string, Index<char>> g_cached_images;

/**
 * Executes a GET request over QNetworkAccessManager and returns raw response bytes.
 * Features a 3-attempt retry loop with linear backoff to gracefully handle socket/SSL
 * handshakes or temporary network contention (especially during concurrent Audacious startup).
 */
static QByteArray msr_fetch_bytes(const QString & url)
{
    if (!qApp)
        return QByteArray();

    for (int attempt = 0; attempt < 3; attempt++) {
        QNetworkAccessManager manager;
        QNetworkRequest request((QUrl(url)));
        request.setHeader(QNetworkRequest::UserAgentHeader, "Audacious-MonsterSiren-Plugin/1.0");
        
        QNetworkReply * reply = manager.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        QByteArray data;
        if (reply->error() == QNetworkReply::NoError) {
            data = reply->readAll();
            reply->deleteLater();
            return data;
        }
        reply->deleteLater();
        if (attempt < 2)
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
    int subtune = 0;
    size_t qpos = s.rfind('?');
    if (qpos != std::string::npos) {
        std::string sub_str = s.substr(qpos + 1);
        char * endp = nullptr;
        long val = std::strtol(sub_str.c_str(), &endp, 10);
        if (endp && *endp == '\0' && val > 0) {
            subtune = (int)val;
            s = s.substr(0, qpos);
        }
    }

    // Strip trailing slashes
    while (!s.empty() && s.back() == '/')
        s.pop_back();

    // Check custom scheme msr://
    if (s.rfind("msr://album/", 0) == 0) {
        result.cid = String(s.substr(12).c_str());
        result.subtune_index = subtune;
        result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
        return result;
    }
    if (s.rfind("msr://song/", 0) == 0) {
        result.cid = String(s.substr(11).c_str());
        result.type = MsrUrlType::Song;
        return result;
    }

    // Check web URL https://monster-siren.hypergryph.com/
    size_t domain_pos = s.find("monster-siren.hypergryph.com");
    if (domain_pos != std::string::npos) {
        // Check for album info
        size_t info_pos = s.find("/info/");
        if (info_pos != std::string::npos) {
            std::string cid_str = s.substr(info_pos + 6);
            result.cid = String(cid_str.c_str());
            result.subtune_index = subtune;
            result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
            return result;
        }
        size_t detail_pos = s.find("/api/album/");
        if (detail_pos != std::string::npos) {
            std::string sub = s.substr(detail_pos + 11);
            size_t slash = sub.find('/');
            if (slash != std::string::npos)
                sub = sub.substr(0, slash);
            result.cid = String(sub.c_str());
            result.subtune_index = subtune;
            result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
            return result;
        }

        // Check for song/music
        size_t music_pos = s.find("/music/");
        if (music_pos != std::string::npos) {
            std::string cid_str = s.substr(music_pos + 7);
            result.cid = String(cid_str.c_str());
            result.type = MsrUrlType::Song;
            return result;
        }
        size_t song_pos = s.find("/song/");
        if (song_pos != std::string::npos || (song_pos = s.find("/api/song/")) != std::string::npos) {
            size_t off = (s.find("/api/song/") != std::string::npos) ? 10 : 6;
            size_t idx = s.find((s.find("/api/song/") != std::string::npos) ? "/api/song/" : "/song/");
            std::string cid_str = s.substr(idx + off);
            result.cid = String(cid_str.c_str());
            result.type = MsrUrlType::Song;
            return result;
        }
    }

    return result;
}

std::vector<MsrAlbum> msr_fetch_albums()
{
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_albums_cached)
            return g_cached_albums;
    }

    QJsonDocument doc = msr_fetch_json("https://monster-siren.hypergryph.com/api/albums");
    std::vector<MsrAlbum> albums;
    if (!doc.isObject())
        return albums;

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

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_albums = albums;
        g_albums_cached = true;
    }

    return albums;
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

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cached_song_details[key] = detail;
    }

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

    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)url));
    if (!bytes.isEmpty()) {
        result.insert(bytes.constData(), 0, bytes.size());
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        Index<char> cache_copy;
        cache_copy.insert(result.begin(), 0, result.len());
        g_cached_images[key] = std::move(cache_copy);
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
}
