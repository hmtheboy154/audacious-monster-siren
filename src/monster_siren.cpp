/**
 * monster_siren.cpp - Shared Backend Library (`libmsr_backend`)
 *
 * Implements communication with the official Hypergryph Monster Siren API
 * (`https://monster-siren.hypergryph.com/api/...`). Provides:
 * 1. Non-blocking, interruptible HTTP/HTTPS transfers via libcurl with bounded timeouts and retry + backoff.
 * 2. JSON deserialization of albums, songs, and artists.
 * 3. Thread-safe in-memory caching (`std::mutex`) for album details, song metadata, and cover art images.
 * 4. URL parsing (`msr_parse_url`) converting custom `msr://` and web URLs into structured routing entities.
 * 5. Background thread registry so plugins can abort and join their workers promptly at shutdown.
 */

#include <curl/curl.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QCryptographicHash>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <libaudcore/runtime.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>
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
struct WorkerEntry {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

static std::mutex g_thread_mutex;
static std::vector<WorkerEntry> g_threads;
static std::atomic<bool> g_shutting_down{false};
static bool g_hooks_registered = false;
static std::once_flag g_curl_init_flag;

static void msr_ensure_curl_init()
{
    std::call_once(g_curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

static void on_cache_clear_hook(void *, void *)
{
    msr_cache_clear();
}

static void on_song_invalidate_hook(void * data, void *)
{
    const char * cid = (const char *)data;
    if (!cid || !cid[0])
        return;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cached_song_details.erase(std::string(cid));
}

static void on_begin_shutdown_hook(void *, void *)
{
    g_shutting_down.store(true, std::memory_order_release);
}

void msr_reset_shutdown()
{
    msr_ensure_curl_init();
    g_shutting_down.store(false, std::memory_order_release);
    if (!g_hooks_registered) {
        hook_associate("msr cache clear", on_cache_clear_hook, nullptr);
        hook_associate("msr song invalidate", on_song_invalidate_hook, nullptr);
        hook_associate("msr begin shutdown", on_begin_shutdown_hook, nullptr);
        g_hooks_registered = true;
    }
}

bool msr_is_shutting_down()
{
    return g_shutting_down.load(std::memory_order_acquire);
}

void msr_notify_app_shutdown()
{
    g_shutting_down.store(true, std::memory_order_release);
    hook_call("msr begin shutdown", nullptr);
}

void msr_run_background(std::function<void()> task)
{
    std::vector<std::thread> finished;
    auto done = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        auto it = g_threads.begin();
        while (it != g_threads.end()) {
            if (it->done->load(std::memory_order_acquire)) {
                finished.push_back(std::move(it->thread));
                it = g_threads.erase(it);
            } else {
                ++it;
            }
        }
        g_threads.push_back({
            std::thread([task = std::move(task), done]() {
                task();
                done->store(true, std::memory_order_release);
            }),
            done
        });
    }

    for (auto & thread : finished) {
        if (thread.joinable()) {
            if (thread.get_id() != std::this_thread::get_id())
                thread.join();
            else
                thread.detach();
        }
    }
}

void msr_begin_shutdown()
{
    g_shutting_down.store(true, std::memory_order_release);
}

void msr_wait_for_background_threads()
{
    msr_begin_shutdown();

    std::vector<WorkerEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        entries.swap(g_threads);
    }

    for (auto & entry : entries)
        if (entry.thread.joinable())
            entry.thread.join();

    if (g_hooks_registered) {
        hook_dissociate("msr cache clear", on_cache_clear_hook);
        hook_dissociate("msr song invalidate", on_song_invalidate_hook);
        hook_dissociate("msr begin shutdown", on_begin_shutdown_hook);
        g_hooks_registered = false;
    }
}

static bool msr_should_abort(const MsrCancelFn & should_cancel)
{
    return g_shutting_down.load(std::memory_order_relaxed) ||
           (should_cancel && should_cancel());
}

static void msr_sleep_interruptible(int total_ms, const MsrCancelFn & should_cancel)
{
    int elapsed = 0;
    while (elapsed < total_ms && !msr_should_abort(should_cancel)) {
        int step = std::min(25, total_ms - elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        elapsed += step;
    }
}

static bool msr_ascii_istarts_with(const char * s, size_t len, const char * prefix)
{
    size_t plen = strlen(prefix);
    if (len < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        unsigned char a = (unsigned char)s[i];
        unsigned char b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool msr_is_nonretryable_http_status(long status)
{
    return status >= 400 && status < 500 && status != 408 && status != 429;
}

static void msr_configure_curl_handle(CURL * easy, const char * url,
                                      long connect_timeout_ms, bool allow_compressed)
{
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 12L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "Audacious-MonsterSiren/1.0");
    if (allow_compressed)
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");

    if (aud_get_bool("use_proxy")) {
        String proxy_host = aud_get_str("proxy_host");
        int proxy_port = aud_get_int("proxy_port");
        bool socks_proxy = aud_get_bool("socks_proxy");
        int socks_type = aud_get_int("socks_type");

        if (proxy_host && proxy_host[0]) {
            curl_easy_setopt(easy, CURLOPT_PROXY, (const char *)proxy_host);
            if (proxy_port > 0)
                curl_easy_setopt(easy, CURLOPT_PROXYPORT, (long)proxy_port);
            curl_easy_setopt(easy, CURLOPT_PROXYTYPE,
                             socks_proxy
                                 ? (socks_type == 0 ? (long)CURLPROXY_SOCKS4A
                                                    : (long)CURLPROXY_SOCKS5_HOSTNAME)
                                 : (long)CURLPROXY_HTTP);

            if (aud_get_bool("use_proxy_auth")) {
                String proxy_user = aud_get_str("proxy_user");
                String proxy_pass = aud_get_str("proxy_pass");
                curl_easy_setopt(easy, CURLOPT_PROXYUSERNAME,
                                 proxy_user ? (const char *)proxy_user : "");
                curl_easy_setopt(easy, CURLOPT_PROXYPASSWORD,
                                 proxy_pass ? (const char *)proxy_pass : "");
            }
        }
    }
}

struct CurlTransferOutcome {
    CURLcode curl_code = CURLE_FAILED_INIT;
    long http_status = 0;
    bool cancelled = false;
    bool timed_out = false;
};

/**
 * Drives a single CURL easy handle on a dedicated CURLM non-blocking poll loop
 * (100 ms max tick) rather than Audacious's Neon VFS transport. This avoids
 * libneon's blocking SSL_peek_ex/SSL_read hang on stalled mid-record TLS
 * streams and guarantees <= 100 ms cancellation on app exit or catalog reload.
 */
static CurlTransferOutcome msr_perform_curl_interruptible(CURL * easy,
                                                          std::atomic<int64_t> & progress_bytes,
                                                          int stall_timeout_ms,
                                                          int max_total_ms,
                                                          const MsrCancelFn & should_cancel)
{
    CurlTransferOutcome outcome;
    CURLM * multi = curl_multi_init();
    if (!multi)
        return outcome;

    if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
        curl_multi_cleanup(multi);
        return outcome;
    }

    auto start_tp = std::chrono::steady_clock::now();
    auto last_progress_tp = start_tp;
    int64_t last_bytes = progress_bytes.load(std::memory_order_relaxed);
    int still_running = 0;

    while (true) {
        if (msr_should_abort(should_cancel)) {
            outcome.cancelled = true;
            break;
        }

        CURLMcode mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK) {
            outcome.curl_code = CURLE_RECV_ERROR;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        int64_t cur_bytes = progress_bytes.load(std::memory_order_relaxed);
        if (cur_bytes != last_bytes) {
            last_bytes = cur_bytes;
            last_progress_tp = now;
        }

        if (!still_running)
            break;

        if (stall_timeout_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_tp).count() >= stall_timeout_ms)
        {
            outcome.timed_out = true;
            outcome.curl_code = CURLE_OPERATION_TIMEDOUT;
            break;
        }

        if (max_total_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_tp).count() >= max_total_ms)
        {
            outcome.timed_out = true;
            outcome.curl_code = CURLE_OPERATION_TIMEDOUT;
            break;
        }

        mc = curl_multi_poll(multi, nullptr, 0, 100, nullptr);
        if (mc != CURLM_OK) {
            outcome.curl_code = CURLE_RECV_ERROR;
            break;
        }
    }

    if (!outcome.cancelled && !outcome.timed_out) {
        int msgs_left = 0;
        while (CURLMsg * msg = curl_multi_info_read(multi, &msgs_left)) {
            if (msg->msg == CURLMSG_DONE)
                outcome.curl_code = msg->data.result;
        }
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &outcome.http_status);
    }

    curl_multi_remove_handle(multi, easy);
    curl_multi_cleanup(multi);
    return outcome;
}

struct CurlBufferSink {
    QByteArray * buf = nullptr;
    std::atomic<int64_t> * progress = nullptr;
    int64_t max_bytes = 256LL * 1024 * 1024;
    bool overflow = false;
};

static size_t msr_curl_progress_header_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    (void)ptr;
    size_t total = size * nmemb;
    auto * progress = static_cast<std::atomic<int64_t> *>(userdata);
    if (progress && total > 0)
        progress->fetch_add((int64_t)total, std::memory_order_relaxed);
    return total;
}

static size_t msr_curl_buffer_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    size_t total = size * nmemb;
    auto * sink = static_cast<CurlBufferSink *>(userdata);
    if (!sink || !sink->buf)
        return 0;
    if ((int64_t)sink->buf->size() + (int64_t)total > sink->max_bytes) {
        sink->overflow = true;
        return 0;
    }
    if (total > 0) {
        sink->buf->append(ptr, (qsizetype)total);
        if (sink->progress)
            sink->progress->fetch_add((int64_t)total, std::memory_order_relaxed);
    }
    return total;
}

static QByteArray msr_fetch_bytes(const QString & url,
                                  int max_total_ms = 0,
                                  const MsrCancelFn & should_cancel = nullptr)
{
    msr_ensure_curl_init();
    QByteArray url_utf8 = url.toUtf8();
    if (url_utf8.isEmpty())
        return QByteArray();

    for (int attempt = 0; attempt < 3; attempt++) {
        if (msr_should_abort(should_cancel))
            return QByteArray();

        CURL * easy = curl_easy_init();
        if (!easy)
            return QByteArray();

        QByteArray out;
        std::atomic<int64_t> progress{0};
        CurlBufferSink sink{&out, &progress, 256LL * 1024 * 1024, false};

        msr_configure_curl_handle(easy, url_utf8.constData(),
                                  max_total_ms > 0 ? 8000L : 10000L, true);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, msr_curl_progress_header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &progress);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, msr_curl_buffer_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);

        int stall_ms = (max_total_ms > 0) ? 10000 : 12000;
        CurlTransferOutcome outcome = msr_perform_curl_interruptible(
            easy, progress, stall_ms, max_total_ms, should_cancel);
        curl_easy_cleanup(easy);

        if (outcome.cancelled)
            return QByteArray();

        if (!sink.overflow && !outcome.timed_out &&
            outcome.curl_code == CURLE_OK &&
            outcome.http_status >= 200 && outcome.http_status < 300 &&
            !out.isEmpty())
        {
            return out;
        }

        if (outcome.timed_out) {
            AUDWARN("MSR: HTTP transfer timed out (attempt %d/3) for %s\n",
                    attempt + 1, url_utf8.constData());
        } else if (outcome.curl_code != CURLE_OK) {
            AUDWARN("MSR: HTTP transfer error '%s' (attempt %d/3) for %s\n",
                    curl_easy_strerror(outcome.curl_code), attempt + 1, url_utf8.constData());
        } else if (outcome.http_status > 0) {
            AUDWARN("MSR: HTTP status %ld (attempt %d/3) for %s\n",
                    outcome.http_status, attempt + 1, url_utf8.constData());
        }

        if (msr_is_nonretryable_http_status(outcome.http_status))
            break;

        if (attempt < 2)
            msr_sleep_interruptible(150 * (attempt + 1), should_cancel);
    }
    return QByteArray();
}

static QJsonDocument msr_fetch_json(const QString & url,
                                    const MsrCancelFn & should_cancel = nullptr)
{
    QByteArray bytes = msr_fetch_bytes(url, 25000, should_cancel);
    if (bytes.isEmpty())
        return QJsonDocument();
    return QJsonDocument::fromJson(bytes);
}

struct CurlRangeSink {
    Index<char> * data = nullptr;
    std::atomic<int64_t> * progress = nullptr;
    int64_t max_bytes = 0;
    int64_t content_range_total = -1;
    bool reached_limit = false;
};

static size_t msr_curl_range_header_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    size_t total = size * nmemb;
    auto * sink = static_cast<CurlRangeSink *>(userdata);
    if (!sink)
        return total;
    if (sink->progress && total > 0)
        sink->progress->fetch_add((int64_t)total, std::memory_order_relaxed);

    if (msr_ascii_istarts_with(ptr, total, "HTTP/")) {
        sink->content_range_total = -1;
    } else if (msr_ascii_istarts_with(ptr, total, "Content-Range:")) {
        std::string line(ptr, total);
        size_t slash = line.find('/');
        if (slash != std::string::npos && slash + 1 < line.size()) {
            const char * num = line.c_str() + slash + 1;
            while (*num == ' ' || *num == '\t')
                num++;
            if (*num >= '0' && *num <= '9') {
                char * endp = nullptr;
                long long parsed = std::strtoll(num, &endp, 10);
                if (parsed > 0)
                    sink->content_range_total = (int64_t)parsed;
            }
        }
    }
    return total;
}

static size_t msr_curl_range_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    size_t total = size * nmemb;
    auto * sink = static_cast<CurlRangeSink *>(userdata);
    if (!sink || !sink->data)
        return 0;

    int64_t cur = sink->data->len();
    int64_t remain = sink->max_bytes - cur;
    if (remain <= 0) {
        sink->reached_limit = true;
        return 0;
    }

    int64_t to_copy = std::min((int64_t)total, remain);
    if (to_copy > 0) {
        sink->data->insert(ptr, -1, (int)to_copy);
        if (sink->progress)
            sink->progress->fetch_add(to_copy, std::memory_order_relaxed);
    }

    if ((int64_t)total > to_copy) {
        sink->reached_limit = true;
        return 0;
    }
    return total;
}

bool msr_http_fetch_range(const char * url, int64_t offset, int64_t max_bytes,
                          Index<char> & out_data, int64_t & out_total_size)
{
    out_data.clear();
    out_total_size = -1;
    if (!url || !url[0] || offset < 0 || max_bytes <= 0 || msr_is_shutting_down())
        return false;

    msr_ensure_curl_init();
    char range_spec[64];
    std::snprintf(range_spec, sizeof(range_spec), "%lld-%lld",
                  (long long)offset, (long long)(offset + max_bytes - 1));

    for (int attempt = 0; attempt < 2; attempt++) {
        if (msr_is_shutting_down())
            return false;

        CURL * easy = curl_easy_init();
        if (!easy)
            return false;

        out_data.clear();
        std::atomic<int64_t> progress{0};
        CurlRangeSink sink{&out_data, &progress, max_bytes, -1, false};

        msr_configure_curl_handle(easy, url, 8000L, false);
        curl_easy_setopt(easy, CURLOPT_RANGE, range_spec);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, msr_curl_range_header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &sink);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, msr_curl_range_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);

        CurlTransferOutcome outcome = msr_perform_curl_interruptible(
            easy, progress, 10000, 20000, nullptr);

        curl_off_t dl_content_len = -1;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &dl_content_len);
        curl_easy_cleanup(easy);

        if (outcome.cancelled)
            return false;

        bool transfer_ok = (outcome.curl_code == CURLE_OK) ||
                           (sink.reached_limit && outcome.curl_code == CURLE_WRITE_ERROR);
        bool status_ok = (outcome.http_status == 206) ||
                         (outcome.http_status == 200 && offset == 0);

        if (!outcome.timed_out && transfer_ok && status_ok && out_data.len() > 0) {
            if (sink.content_range_total > 0)
                out_total_size = sink.content_range_total;
            else if (outcome.http_status == 200 && dl_content_len > 0)
                out_total_size = (int64_t)dl_content_len;
            return true;
        }

        if (msr_is_nonretryable_http_status(outcome.http_status))
            break;

        if (attempt < 1)
            msr_sleep_interruptible(150, nullptr);
    }

    out_data.clear();
    return false;
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
        std::string cid = cid_at(s, 12);
        if (cid.empty())
            return result;
        result.cid = String(cid.c_str());
        result.subtune_index = subtune;
        result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
        return result;
    }
    if (s.rfind("msr://song/", 0) == 0) {
        std::string cid = cid_at(s, 11);
        if (cid.empty())
            return result;
        result.cid = String(cid.c_str());
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
            std::string cid = cid_at(s, start + strlen(marker));
            if (cid.empty())
                return result;
            result.cid = String(cid.c_str());
            result.subtune_index = subtune;
            if (album)
                result.type = (subtune > 0) ? MsrUrlType::AlbumTrack : MsrUrlType::Album;
            else
                result.type = MsrUrlType::Song;
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
    } else if (!strncmp((const char *)dir, "file://", 7)) {
        StringBuf local = uri_to_filename((const char *)dir);
        if (local && local[0]) {
            aud_set_str("monster-siren", "cache_dir", local);
            dir = String(local);
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

static bool msr_atomic_write(const char * filepath, const char * data, int64_t len)
{
    if (!filepath || !data || len <= 0)
        return false;

    QSaveFile file(QString::fromUtf8(filepath));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(data, len) != len) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

static bool msr_nonempty_file_exists(const char * filepath)
{
    QFileInfo info(QString::fromUtf8(filepath));
    return info.exists() && info.isFile() && info.size() > 0;
}

static String msr_audio_ext_for_url(const String & sourceUrl)
{
    // Strip query parameters before finding extension (CDN URLs contain ?token=...)
    std::string url_clean((const char *)sourceUrl);
    size_t qmark = url_clean.find('?');
    if (qmark != std::string::npos)
        url_clean = url_clean.substr(0, qmark);
    const char * dot = strrchr(url_clean.c_str(), '.');
    return (dot && strlen(dot) <= 5) ? String(dot) : String(".mp3");
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
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    msr_atomic_write((const char *)filepath, json.constData(), json.size());
}

bool msr_find_cached_art(const String & cid_or_url, String & out_path)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_images") || !cid_or_url)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf art_dir = filename_build({cache_dir, "art"});
    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({art_dir, StringBuf(str_concat({"cover_", hash.toUtf8().constData(), ".jpg"}))});
    if (msr_nonempty_file_exists((const char *)filepath)) {
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
    msr_atomic_write((const char *)filepath, data.begin(), data.len());
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
        if (msr_nonempty_file_exists((const char *)filepath)) {
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

    String ext = msr_audio_ext_for_url(sourceUrl);
    StringBuf filepath = filename_build({audio_dir, StringBuf(str_concat({"audio_", cid, ext}))});
    msr_atomic_write((const char *)filepath, data.begin(), data.len());
}

bool msr_find_cached_lyrics(const String & cid_or_url, String & out_path)
{
    if (!aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio") || !cid_or_url)
        return false;

    String cache_dir = msr_get_cache_dir();
    StringBuf lyrics_dir = filename_build({cache_dir, "lyrics"});
    QString hash = msr_url_to_hash((const char *)cid_or_url);
    StringBuf filepath = filename_build({lyrics_dir, StringBuf(str_concat({"lyric_", hash.toUtf8().constData(), ".lrc"}))});
    if (msr_nonempty_file_exists((const char *)filepath)) {
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
    msr_atomic_write((const char *)filepath, (const char *)lyrics_text, strlen((const char *)lyrics_text));
}

static std::mutex g_inflight_audio_mutex;
static std::set<std::string> g_inflight_audio_cids;

struct CurlFileSink {
    QSaveFile * file = nullptr;
    std::atomic<int64_t> * progress = nullptr;
    int64_t written = 0;
    bool write_ok = true;
};

static size_t msr_curl_file_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    size_t total = size * nmemb;
    auto * sink = static_cast<CurlFileSink *>(userdata);
    if (!sink || !sink->file)
        return 0;
    if (total == 0)
        return 0;
    if (sink->file->write(ptr, (qint64)total) != (qint64)total) {
        sink->write_ok = false;
        return 0;
    }
    sink->written += (int64_t)total;
    if (sink->progress)
        sink->progress->fetch_add((int64_t)total, std::memory_order_relaxed);
    return total;
}

void msr_fetch_and_cache_audio_background(const String & cid, const String & sourceUrl)
{
    if (!cid || !sourceUrl || !aud_get_bool("monster-siren", "enable_cache") || !aud_get_bool("monster-siren", "cache_audio"))
        return;

    String existing_path;
    if (msr_find_cached_audio(cid, existing_path))
        return;

    std::string cid_key((const char *)cid);
    {
        std::lock_guard<std::mutex> lock(g_inflight_audio_mutex);
        if (!g_inflight_audio_cids.insert(cid_key).second)
            return;
    }

    struct InflightGuard {
        std::string key;
        ~InflightGuard()
        {
            std::lock_guard<std::mutex> lock(g_inflight_audio_mutex);
            g_inflight_audio_cids.erase(key);
        }
    } guard{cid_key};

    if (msr_find_cached_audio(cid, existing_path))
        return;

    msr_ensure_curl_init();
    String cache_dir = msr_get_cache_dir();
    StringBuf audio_dir = filename_build({cache_dir, "audio"});
    QDir().mkpath(QString::fromUtf8((const char *)audio_dir));

    String ext = msr_audio_ext_for_url(sourceUrl);
    StringBuf filepath = filename_build({audio_dir, StringBuf(str_concat({"audio_", cid, ext}))});

    for (int attempt = 0; attempt < 3 && !msr_is_shutting_down(); attempt++) {
        QSaveFile out(QString::fromUtf8((const char *)filepath));
        if (!out.open(QIODevice::WriteOnly))
            return;

        CURL * easy = curl_easy_init();
        if (!easy) {
            out.cancelWriting();
            return;
        }

        std::atomic<int64_t> progress{0};
        CurlFileSink sink{&out, &progress, 0, true};

        msr_configure_curl_handle(easy, (const char *)sourceUrl, 10000L, false);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, msr_curl_progress_header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &progress);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, msr_curl_file_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);

        CurlTransferOutcome outcome = msr_perform_curl_interruptible(
            easy, progress, 15000, 0, nullptr);

        curl_off_t expected = -1;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected);
        curl_easy_cleanup(easy);

        bool complete = !outcome.cancelled && !outcome.timed_out &&
                        sink.write_ok && sink.written > 0 &&
                        outcome.curl_code == CURLE_OK &&
                        outcome.http_status >= 200 && outcome.http_status < 300 &&
                        (expected <= 0 || sink.written >= (int64_t)expected);
        if (complete && out.commit())
            return;
        out.cancelWriting();

        if (outcome.cancelled || msr_is_nonretryable_http_status(outcome.http_status))
            return;

        if (attempt < 2)
            msr_sleep_interruptible(150 * (attempt + 1), nullptr);
    }
}

std::vector<MsrAlbum> msr_fetch_albums(bool force_network, const MsrCancelFn & should_cancel)
{
    if (msr_should_abort(should_cancel))
        return {};

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

    QJsonDocument doc = msr_fetch_json("https://monster-siren.hypergryph.com/api/albums", should_cancel);
    std::vector<MsrAlbum> albums;
    if (!doc.isObject()) {
        if (!msr_should_abort(should_cancel) && !force_network && msr_load_catalog_from_cache(albums)) {
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

    if (!albums.empty()) {
        msr_save_catalog_to_cache(albums);
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

    QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    msr_atomic_write((const char *)filepath, json.constData(), json.size());
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

    QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    msr_atomic_write((const char *)filepath, json.constData(), json.size());
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
    hook_call("msr song invalidate", (void *)(const char *)cid);
}

MsrAlbum msr_fetch_album_detail(const String & cid)
{
    if (!cid || msr_is_shutting_down())
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
    if (!cid || msr_is_shutting_down())
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

Index<char> msr_fetch_image(const String & url, const MsrCancelFn & should_cancel)
{
    Index<char> result;
    if (!url || !url[0] || msr_should_abort(should_cancel))
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

    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)url), 0, should_cancel);
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
    if (!url || !url[0] || msr_is_shutting_down())
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
    QByteArray bytes = msr_fetch_bytes(QString::fromUtf8((const char *)url), 25000);
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
    hook_call("msr cache clear", nullptr);
}
