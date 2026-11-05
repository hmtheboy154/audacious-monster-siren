/**
 * msr_input.cpp - Audacious Input Plugin for Monster Siren (Arknights Soundtrack)
 *
 * This plugin intercepts URLs starting with `msr://` and standard web URLs from
 * `monster-siren.hypergryph.com`. It acts as a bridge:
 * 1. Probing & expanding albums into individual subtune track entries (FlagSubtunes).
 * 2. Fetching track metadata, cover art, and remote durations (read_tag).
 * 3. Resolving stream URLs and delegating audio decoding to Audacious's native decoders (play).
 */

#include "monster_siren.h"
#include <libaudcore/plugin.h>
#include <libaudcore/i18n.h>
#include <libaudcore/probe.h>
#include <libaudcore/vfs.h>
#include <libaudcore/runtime.h>
#include <libaudcore/audstrings.h>
#include <libaudcore/preferences.h>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstring>

static const char * const msr_schemes[] = {"msr", nullptr};

/* Defaults for the shared [monster-siren] settings that this module owns.
 * The UI modules only register the view-mode options, so registering the
 * caching keys here keeps them defined whenever the input plugin (which
 * actually executes the caching) is present. */
static const char * const msr_input_defaults[] = {
    "enable_cache", "FALSE",
    "cache_dir", "",
    "cache_catalog", "TRUE",
    "cache_images", "TRUE",
    "cache_audio", "TRUE",
    "manual_sync", "FALSE",
    nullptr
};

static const PreferencesWidget msr_input_widgets[] = {
    WidgetLabel(N_("<b>Disk Caching &amp; Offline Indexing</b>")),
    WidgetCheck(N_("Enable Offline Disk Cache & Indexing"),
                WidgetBool("monster-siren", "enable_cache")),
    WidgetFileEntry(N_("Cache Directory"),
                    WidgetString("monster-siren", "cache_dir"),
                    {FileSelectMode::Folder}, WIDGET_CHILD),
    WidgetCheck(N_("Cache Catalog Metadata Index"),
                WidgetBool("monster-siren", "cache_catalog"), WIDGET_CHILD),
    WidgetCheck(N_("Cache Album Cover Art Images"),
                WidgetBool("monster-siren", "cache_images"), WIDGET_CHILD),
    WidgetCheck(N_("Cache Audio Tracks & Lyrics during playback"),
                WidgetBool("monster-siren", "cache_audio"), WIDGET_CHILD),
    WidgetCheck(N_("Manual Sync Mode (Load only from disk until forced refresh)"),
                WidgetBool("monster-siren", "manual_sync"), WIDGET_CHILD)
};

static const PluginPreferences msr_input_prefs = {.widgets = msr_input_widgets, .init = nullptr, .apply = nullptr, .cleanup = nullptr};

static uint32_t read_le32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t * p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * Parses WAV (RIFF), FLAC (STREAMINFO), or MP3 (Xing/VBRI VBR or CBR + HTTP
 * Content-Length) headers from an open VFSFile using bounded reads so probing
 * a remote stream never seeks to the end of the file or downloads the full
 * track just to learn its duration.
 */
static bool probe_header_length(VFSFile & file, Tuple & target_tuple)
{
    uint8_t buf[65536];
    int64_t n = file.fread(buf, 1, sizeof(buf));
    if (n < 12)
        return false;

    int64_t total_size = file.fsize();

    // 1. RIFF/WAVE
    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        int64_t pos = 12;
        uint32_t byte_rate = 0, data_size = 0;
        bool have_fmt = false;

        while (pos + 8 <= n) {
            uint32_t chunk_size = read_le32(buf + pos + 4);
            if (memcmp(buf + pos, "fmt ", 4) == 0 && chunk_size >= 16 && pos + 8 + 16 <= n) {
                byte_rate = read_le32(buf + pos + 16);
                have_fmt = true;
            } else if (memcmp(buf + pos, "data", 4) == 0) {
                data_size = chunk_size;
                break;
            }
            pos += 8 + (int64_t)chunk_size + (chunk_size & 1);
        }

        if (have_fmt && byte_rate > 0) {
            int64_t pcm_bytes = data_size;
            if (pcm_bytes <= 0 && total_size > pos)
                pcm_bytes = total_size - pos;
            if (pcm_bytes > 0) {
                target_tuple.set_int(Tuple::Length, (int)(pcm_bytes * 1000 / byte_rate));
                target_tuple.set_int(Tuple::Bitrate, (int)((int64_t)byte_rate * 8 / 1000));
                return true;
            }
        }
        return false;
    }

    // Skip optional leading ID3v2 tag (bounded to <= 1 MiB on remote streams)
    int64_t audio_offset = 0;
    const uint8_t * p = buf;
    int64_t avail = n;

    if (memcmp(buf, "ID3", 3) == 0 &&
        (buf[6] & 0x80) == 0 && (buf[7] & 0x80) == 0 &&
        (buf[8] & 0x80) == 0 && (buf[9] & 0x80) == 0)
    {
        int64_t tag_size = ((int64_t)buf[6] << 21) | ((int64_t)buf[7] << 14) |
                           ((int64_t)buf[8] << 7) | (int64_t)buf[9];
        audio_offset = 10 + tag_size + ((buf[5] & 0x10) ? 10 : 0);
        if (audio_offset < n) {
            p = buf + audio_offset;
            avail = n - audio_offset;
        } else if (audio_offset <= 1024 * 1024 && file.fseek(audio_offset, VFS_SEEK_SET) == 0) {
            n = file.fread(buf, 1, sizeof(buf));
            p = buf;
            avail = n;
        } else {
            return false;
        }
    }

    // 2. FLAC (STREAMINFO metadata block)
    if (avail >= 42 && memcmp(p, "fLaC", 4) == 0 && (p[4] & 0x7F) == 0) {
        const uint8_t * s = p + 8;
        uint32_t sample_rate = ((uint32_t)s[10] << 12) | ((uint32_t)s[11] << 4) |
                               ((uint32_t)s[12] >> 4);
        uint64_t total_samples = (((uint64_t)(s[13] & 0x0F)) << 32) |
                                 ((uint64_t)s[14] << 24) | ((uint64_t)s[15] << 16) |
                                 ((uint64_t)s[16] << 8) | (uint64_t)s[17];
        if (sample_rate > 0 && total_samples > 0) {
            int length_ms = (int)(total_samples * 1000 / sample_rate);
            if (length_ms > 0) {
                target_tuple.set_int(Tuple::Length, length_ms);
                if (total_size > audio_offset)
                    target_tuple.set_int(Tuple::Bitrate, (int)((total_size - audio_offset) * 8 / length_ms));
                return true;
            }
        }
        return false;
    }

    // 3. MPEG Audio (MP3 Layer III / Layer II)
    static const int mpeg1_l3_br[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int mpeg1_l2_br[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const int mpeg2_l3_br[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int mpeg1_sr[4] = {44100, 48000, 32000, 0};
    static const int mpeg2_sr[4] = {22050, 24000, 16000, 0};
    static const int mpeg25_sr[4] = {11025, 12000, 8000, 0};

    for (int64_t i = 0; i + 4 <= avail; i++) {
        if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0)
            continue;

        int ver = (p[i + 1] >> 3) & 0x03;
        int layer = (p[i + 1] >> 1) & 0x03;
        int br_idx = (p[i + 2] >> 4) & 0x0F;
        int sr_idx = (p[i + 2] >> 2) & 0x03;
        int pad = (p[i + 2] >> 1) & 0x01;
        int chan_mode = (p[i + 3] >> 6) & 0x03;

        if (ver == 1 || (layer != 1 && layer != 2) || br_idx == 0 || br_idx == 15 || sr_idx == 3)
            continue;

        int sr = (ver == 3) ? mpeg1_sr[sr_idx] : (ver == 2 ? mpeg2_sr[sr_idx] : mpeg25_sr[sr_idx]);
        int br = (ver == 3) ? (layer == 1 ? mpeg1_l3_br[br_idx] : mpeg1_l2_br[br_idx])
                            : mpeg2_l3_br[br_idx];
        int spf = (layer == 1 && ver != 3) ? 576 : 1152;
        if (sr <= 0 || br <= 0)
            continue;

        int frame_size = (spf / 8 * br * 1000) / sr + pad;
        if (frame_size < 24)
            continue;
        if (i + frame_size + 2 <= avail &&
            (p[i + frame_size] != 0xFF || (p[i + frame_size + 1] & 0xE0) != 0xE0))
            continue;

        uint32_t total_frames = 0;
        if (layer == 1) {
            int side_info = (ver == 3) ? ((chan_mode == 3) ? 17 : 32)
                                       : ((chan_mode == 3) ? 9 : 17);
            int64_t xing_pos = i + 4 + side_info;
            if (xing_pos + 12 <= avail &&
                (memcmp(p + xing_pos, "Xing", 4) == 0 || memcmp(p + xing_pos, "Info", 4) == 0))
            {
                uint32_t flags = read_be32(p + xing_pos + 4);
                if (flags & 0x01)
                    total_frames = read_be32(p + xing_pos + 8);
            }
            int64_t vbri_pos = i + 4 + 32;
            if (total_frames == 0 && vbri_pos + 18 <= avail && memcmp(p + vbri_pos, "VBRI", 4) == 0)
                total_frames = read_be32(p + vbri_pos + 14);
        }

        if (total_frames > 0) {
            int length_ms = (int)((int64_t)total_frames * spf * 1000 / sr);
            if (length_ms > 0) {
                target_tuple.set_int(Tuple::Length, length_ms);
                int kbps = (total_size > audio_offset + i)
                               ? (int)((total_size - audio_offset - i) * 8 / length_ms)
                               : br;
                target_tuple.set_int(Tuple::Bitrate, kbps);
                return true;
            }
        } else if (total_size > audio_offset + i) {
            int64_t stream_bytes = total_size - audio_offset - i;
            int length_ms = (int)(stream_bytes * 8 / br);
            if (length_ms > 0) {
                target_tuple.set_int(Tuple::Length, length_ms);
                target_tuple.set_int(Tuple::Bitrate, br);
                return true;
            }
        }
        break;
    }

    return false;
}

/**
 * Helper function to probe audio stream duration over VFS.
 * Returns false only when opening a remote stream failed (e.g. expired CDN
 * token), allowing callers to refresh song detail and retry once.
 */
static bool probe_remote_length(const String & cid, const String & sourceUrl, Tuple & target_tuple)
{
    if (!sourceUrl || !sourceUrl[0])
        return true;

    String probe_url = sourceUrl;
    String cached_path;
    bool use_cache = cid && msr_find_cached_audio(cid, cached_path);
    if (use_cache) {
        probe_url = String(StringBuf(str_concat({"file://", cached_path})));
        AUDINFO("MSR: probe_remote_length using local audio cache for cid %s -> %s\n", (const char *)cid, (const char *)probe_url);
    } else {
        AUDINFO("MSR: probe_remote_length probing remote URL for cid %s -> %s\n", (const char *)cid, (const char *)probe_url);
    }

    VFSFile probe_file(probe_url, "r");
    if (!probe_file) {
        AUDWARN("MSR: probe_remote_length failed to open VFSFile for %s\n", (const char *)probe_url);
        return use_cache;
    }
    if (probe_header_length(probe_file, target_tuple)) {
        AUDINFO("MSR: probe_remote_length (header parse) success for cid %s -> Length: %d ms, Bitrate: %d\n",
                (const char *)cid, target_tuple.get_int(Tuple::Length), target_tuple.get_int(Tuple::Bitrate));
        return true;
    }

    /* Full decoder probing is only safe for local files; on a remote URL the
     * decoder's end-of-stream seek would download the whole track. */
    if (!use_cache) {
        AUDINFO("MSR: probe_remote_length: unrecognized remote header for %s, length stays unknown\n", (const char *)probe_url);
        return true;
    }

    if (probe_file.fseek(0, VFS_SEEK_SET) != 0) {
        /* proceed anyway if seek fails */
    }
    PluginHandle * decoder = aud_file_find_decoder(probe_url, true, probe_file);
    if (!decoder)
        decoder = aud_file_find_decoder(probe_url, false, probe_file);
    if (!decoder) {
        AUDWARN("MSR: probe_remote_length found no decoder for %s\n", (const char *)probe_url);
        return true;
    }
    auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
    if (!ip)
        return true;
    Tuple stream_tuple;
    if (ip->read_tag(probe_url, probe_file, stream_tuple, nullptr)) {
        if (stream_tuple.get_int(Tuple::Length) > 0)
            target_tuple.set_int(Tuple::Length, stream_tuple.get_int(Tuple::Length));
        if (stream_tuple.get_int(Tuple::Bitrate) > 0)
            target_tuple.set_int(Tuple::Bitrate, stream_tuple.get_int(Tuple::Bitrate));
        AUDINFO("MSR: probe_remote_length success for cid %s -> Length: %d ms, Bitrate: %d\n", (const char *)cid, target_tuple.get_int(Tuple::Length), target_tuple.get_int(Tuple::Bitrate));
    }
    return true;
}

static void ensure_song_duration(MsrSongDetail & song, Tuple & tuple, bool playback_prep)
{
    if (tuple.get_int(Tuple::Length) > 0 || !song.cid)
        return;

    if (song.duration_ms > 0) {
        tuple.set_int(Tuple::Length, song.duration_ms);
        return;
    }

    if (!song.sourceUrl || !song.sourceUrl[0])
        return;

    AUDINFO("MSR: Probing duration for cid %s (%s)\n", (const char *)song.cid,
            playback_prep ? "playback prep" : "catalog scan");
    if (!probe_remote_length(song.cid, song.sourceUrl, tuple)) {
        AUDWARN("MSR: Duration probe failed to open %s (likely expired token); refreshing song detail\n",
                (const char *)song.sourceUrl);
        msr_invalidate_song_detail(song.cid);
        song = msr_fetch_song_detail(song.cid);
        if (song.sourceUrl && song.sourceUrl[0])
            probe_remote_length(song.cid, song.sourceUrl, tuple);
    }

    if (tuple.get_int(Tuple::Length) > 0) {
        song.duration_ms = tuple.get_int(Tuple::Length);
        msr_save_song_detail_to_cache(song);
    }
}

class MsrInputPlugin : public InputPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Monster Siren Input Plugin"),
        PACKAGE,
        nullptr,
        &msr_input_prefs,
        0
    };

    constexpr MsrInputPlugin()
        : InputPlugin(info, InputInfo(FlagSubtunes)
                                .with_schemes(msr_schemes)
                                .with_priority(1))
    {
    }

    bool init() override
    {
        msr_reset_shutdown();
        aud_config_set_defaults("monster-siren", msr_input_defaults);
        return true;
    }

    /* Audacious dlcloses plugin modules at exit; join any in-flight
     * background cache downloads (after aborting their transfer loop) so they
     * never execute into freed transport/OpenSSL code after module unload. */
    void cleanup() override
    {
        msr_wait_for_background_threads();
    }

    /**
     * Checks whether Audacious should route the URL to this plugin.
     * Intercepts both custom `msr://` scheme URLs and standard Hypergryph web URLs.
     */
    bool is_our_file(const char * filename, VFSFile & file) override
    {
        (void)file;
        MsrUrlParsed parsed = msr_parse_url(filename);
        return parsed.type != MsrUrlType::Unknown;
    }

    /**
     * Reads track metadata, album cover art, and subtune structure.
     * Called when Audacious adds URLs to the playlist or refreshes metadata.
     */
    bool read_tag(const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image) override
    {
        (void)file;
        MsrUrlParsed parsed = msr_parse_url(filename);
        if (parsed.type == MsrUrlType::Unknown)
            return false;

        // Handle Album URLs (e.g. msr://album/8929): expand into N subtune entries
        if (parsed.type == MsrUrlType::Album) {
            MsrAlbum album = msr_fetch_album_detail(parsed.cid);
            if (!album.cid || album.tracks.empty())
                return false;

            int n = (int)album.tracks.size();
            std::vector<short> subs(n);
            for (int i = 0; i < n; i++)
                subs[i] = (short)(i + 1);

            tuple.set_subtunes((short)n, subs.data());
            tuple.set_str(Tuple::Title, album.name);
            tuple.set_str(Tuple::Artist, album.artistes);
            tuple.set_str(Tuple::Album, album.name);

            if (image && album.coverUrl)
                *image = msr_fetch_image(album.coverUrl);

            return true;
        }

        // Handle specific Album Track (e.g. msr://album/8929?1)
        if (parsed.type == MsrUrlType::AlbumTrack) {
            MsrAlbum album = msr_fetch_album_detail(parsed.cid);
            if (!album.cid || parsed.subtune_index <= 0 || parsed.subtune_index > (int)album.tracks.size())
                return false;

            const MsrTrack & track = album.tracks[parsed.subtune_index - 1];
            tuple.set_str(Tuple::Title, track.name);
            tuple.set_str(Tuple::Artist, track.artistes);
            tuple.set_str(Tuple::Album, album.name);
            tuple.set_int(Tuple::Track, parsed.subtune_index);

            if (image) {
                AUDINFO("MSR: read_tag (PLAYBACK/FULL mode, image!=null) for %s (cid: %s)\n", filename, (const char *)track.cid);
                if (album.coverUrl && album.coverUrl[0])
                    *image = msr_fetch_image(album.coverUrl);
            } else {
                AUDINFO("MSR: read_tag (SCAN/FAST mode, image=null) for %s (cid: %s)\n", filename, (const char *)track.cid);
            }

            // Probe track duration: read it from memory/disk cache when already
            // known, otherwise probe the stream (using the local audio cache
            // file when one exists). Probing during the catalog scan (image ==
            // nullptr) as well as during playback prep makes Tuple::Length
            // deterministic, which is what enables the seekbar / timestamp
            // seeking in Audacious.
            MsrSongDetail song = msr_fetch_song_detail(track.cid);
            ensure_song_duration(song, tuple, image != nullptr);

            // Populate lyrics (Tuple::Lyrics) in both scan and playback modes so
            // Audacious's lyric plugins (lyrics-qt, lyrics-gtk) show Monster
            // Siren's own lyrics instead of falling back to third-party sources.
            // A placeholder is always set when MSR provides no lyrics.
            if (!tuple.get_str(Tuple::Lyrics)) {
                if (song.lyricUrl && song.lyricUrl[0]) {
                    String cached_lyric_path;
                    if (msr_find_cached_lyrics(song.lyricUrl, cached_lyric_path))
                        AUDINFO("MSR: Loading lyrics from local cache in read_tag for cid %s\n", (const char *)track.cid);
                    else
                        AUDINFO("MSR: Fetching remote lyrics in read_tag for cid %s\n", (const char *)track.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                }
                if (!tuple.get_str(Tuple::Lyrics))
                    tuple.set_str(Tuple::Lyrics, String("[No lyrics provided by Monster Siren]"));
            }

            return true;
        }

        // Handle standalone Song URLs (e.g. msr://song/125021)
        if (parsed.type == MsrUrlType::Song) {
            MsrSongDetail song = msr_fetch_song_detail(parsed.cid);
            if (!song.cid)
                return false;

            tuple.set_str(Tuple::Title, song.name);
            tuple.set_str(Tuple::Artist, song.artists);

            // Fallback cover art logic: if song doesn't have an MV cover, use parent album cover art
            String coverUrl = (song.mvCoverUrl && song.mvCoverUrl[0]) ? song.mvCoverUrl : String();
            if (song.albumCid && song.albumCid[0]) {
                MsrAlbum album = msr_fetch_album_detail(song.albumCid);
                if (album.name && album.name[0])
                    tuple.set_str(Tuple::Album, album.name);
                if ((!coverUrl || !coverUrl[0]) && album.coverUrl && album.coverUrl[0])
                    coverUrl = album.coverUrl;
            }

            if (image) {
                AUDINFO("MSR: read_tag (PLAYBACK/FULL mode, image!=null) for standalone song %s (cid: %s)\n", filename, (const char *)song.cid);
                if (coverUrl && coverUrl[0])
                    *image = msr_fetch_image(coverUrl);
            } else {
                AUDINFO("MSR: read_tag (SCAN/FAST mode, image=null) for standalone song %s (cid: %s)\n", filename, (const char *)song.cid);
            }

            ensure_song_duration(song, tuple, image != nullptr);

            if (!tuple.get_str(Tuple::Lyrics)) {
                if (song.lyricUrl && song.lyricUrl[0]) {
                    String cached_lyric_path;
                    if (msr_find_cached_lyrics(song.lyricUrl, cached_lyric_path))
                        AUDINFO("MSR: Loading lyrics from local cache in read_tag for standalone song cid %s\n", (const char *)song.cid);
                    else
                        AUDINFO("MSR: Fetching remote lyrics in read_tag for standalone song cid %s\n", (const char *)song.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                }
                if (!tuple.get_str(Tuple::Lyrics))
                    tuple.set_str(Tuple::Lyrics, String("[No lyrics provided by Monster Siren]"));
            }

            return true;
        }

        return false;
    }

    /**
     * Resolves the remote audio stream URL (hycdn.cn) and delegates actual audio
     * decoding/playback to an appropriate underlying decoder plugin.
     * Supports transparent offline playback from local disk cache when available.
     */
    bool play(const char * filename, VFSFile & file) override
    {
        MsrUrlParsed parsed = msr_parse_url(filename);
        String song_cid;

        if (parsed.type == MsrUrlType::Song) {
            song_cid = parsed.cid;
        } else if (parsed.type == MsrUrlType::AlbumTrack) {
            MsrAlbum album = msr_fetch_album_detail(parsed.cid);
            if (parsed.subtune_index > 0 && parsed.subtune_index <= (int)album.tracks.size())
                song_cid = album.tracks[parsed.subtune_index - 1].cid;
        }

        if (!song_cid) {
            AUDERR("Failed to resolve song CID for %s\n", filename);
            return false;
        }

        MsrSongDetail song = msr_fetch_song_detail(song_cid);
        if (!song.sourceUrl) {
            AUDERR("Failed to fetch audio stream URL for CID %s\n", (const char *)song_cid);
            return false;
        }

        String stream_url = song.sourceUrl;
        String cached_path;
        bool using_cached_audio = msr_find_cached_audio(song.cid, cached_path);
        if (using_cached_audio) {
            AUDINFO("Playing Monster Siren track %s directly from offline disk cache: %s\n", (const char *)song.name, (const char *)cached_path);
            stream_url = String(StringBuf(str_concat({"file://", cached_path})));
        }

        AUDINFO("Streaming Monster Siren track %s from URL: %s\n", (const char *)song.name, (const char *)stream_url);

        VFSFile source_file(stream_url, "r");
        PluginHandle * decoder = nullptr;
        if (source_file) {
            decoder = aud_file_find_decoder(stream_url, true, source_file);
            if (!decoder)
                decoder = aud_file_find_decoder(stream_url, false, source_file);
        }

        // If a cached local audio file failed to open/decode, purge it and fall back to remote stream
        if ((!source_file || !decoder) && using_cached_audio) {
            AUDWARN("Cached audio file %s could not be decoded; removing and falling back to remote stream\n",
                    (const char *)cached_path);
            std::remove((const char *)cached_path);
            using_cached_audio = false;
            stream_url = song.sourceUrl;
            source_file = VFSFile(stream_url, "r");
            if (source_file) {
                decoder = aud_file_find_decoder(stream_url, true, source_file);
                if (!decoder)
                    decoder = aud_file_find_decoder(stream_url, false, source_file);
            }
        }

        // If connection or decoder check failed on remote stream (e.g. HTTP 403 CDN token expired),
        // invalidate cached song detail and refetch from API for a brand new sourceUrl + token!
        if ((!source_file || !decoder) && !using_cached_audio) {
            AUDWARN("Stream open/decode failed for %s (likely 403 expired token). Refetching fresh song detail...\n", (const char *)stream_url);
            msr_invalidate_song_detail(song.cid);
            song = msr_fetch_song_detail(song.cid);
            if (song.sourceUrl && song.sourceUrl[0]) {
                stream_url = song.sourceUrl;
                source_file = VFSFile(stream_url, "r");
                if (source_file) {
                    decoder = aud_file_find_decoder(stream_url, true, source_file);
                    if (!decoder)
                        decoder = aud_file_find_decoder(stream_url, false, source_file);
                }
            }
        } else {
            AUDINFO("MSR: Playing audio stream: %s\n", (const char *)stream_url);
        }

        if (!decoder) {
            AUDERR("Could not find decoder plugin for %s\n", (const char *)stream_url);
            return false;
        }

        if (!using_cached_audio && aud_get_bool("monster-siren", "enable_cache") &&
            aud_get_bool("monster-siren", "cache_audio") && song.sourceUrl && song.sourceUrl[0])
        {
            msr_run_background([cid = String(song.cid), sourceUrl = String(song.sourceUrl)]() {
                msr_fetch_and_cache_audio_background(cid, sourceUrl);
            });
        }

        auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
        if (!ip)
            return false;

        // Ensure playback tuple has duration/bitrate/lyrics populated before starting stream
        Tuple t;
        read_tag(filename, file, t, nullptr);
        if (t.get_int(Tuple::Length) <= 0) {
            probe_remote_length(song.cid, stream_url, t);
            if (t.get_int(Tuple::Length) > 0 && song.duration_ms <= 0) {
                song.duration_ms = t.get_int(Tuple::Length);
                msr_save_song_detail_to_cache(song);
            }
        }

        if (!t.get_str(Tuple::Lyrics) && song.lyricUrl && song.lyricUrl[0]) {
            AUDINFO("MSR: Fetching/verifying lyrics in play() for cid %s\n", (const char *)song.cid);
            String lyrics = msr_fetch_lyrics(song.lyricUrl);
            if (lyrics && lyrics[0])
                t.set_str(Tuple::Lyrics, lyrics);
        }
        if (!t.get_str(Tuple::Lyrics))
            t.set_str(Tuple::Lyrics, String("[No lyrics provided by Monster Siren]"));

        t.set_int(Tuple::StartTime, 0);
        t.set_str(Tuple::AudioFile, filename);

        if (t.valid())
            set_playback_tuple(std::move(t));

        // Always open a fresh, clean VFSFile from byte 0 for audio streaming/playback so seeking works reliably
        AUDINFO("MSR: Handing off to underlying decoder %s with clean VFSFile\n", aud_plugin_get_name(decoder));
        VFSFile clean_file(stream_url, "r");
        if (!clean_file) {
            AUDERR("Could not open VFS connection to %s for playback\n", (const char *)stream_url);
            return false;
        }

        return ip->play(stream_url, clean_file);
    }
};

EXPORT MsrInputPlugin aud_plugin_instance;
