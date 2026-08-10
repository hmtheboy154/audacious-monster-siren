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
#include <vector>
#include <thread>

static const char * const msr_schemes[] = {"msr", nullptr};

/**
 * Helper function to probe remote audio stream duration over VFS.
 * By querying the underlying audio decoder (e.g. sndfile or ffaudio) before playback,
 * we ensure Tuple::Length is set (> 0). This enables Audacious to display exact track
 * timestamps on the seekbar and permits seeking via HTTP byte-range requests.
 */
static void probe_remote_length(const String & cid, const String & sourceUrl, Tuple & target_tuple)
{
    if (!sourceUrl || !sourceUrl[0])
        return;

    String probe_url = sourceUrl;
    String cached_path;
    if (cid && msr_find_cached_audio(cid, cached_path)) {
        probe_url = String(StringBuf(str_concat({"file://", cached_path})));
        AUDINFO("MSR: probe_remote_length using local audio cache for cid %s -> %s\n", (const char *)cid, (const char *)probe_url);
    } else {
        AUDINFO("MSR: probe_remote_length probing remote URL for cid %s -> %s\n", (const char *)cid, (const char *)probe_url);
    }

    VFSFile probe_file(probe_url, "r");
    if (!probe_file) {
        AUDWARN("MSR: probe_remote_length failed to open VFSFile for %s\n", (const char *)probe_url);
        return;
    }
    PluginHandle * decoder = aud_file_find_decoder(probe_url, true, probe_file);
    if (!decoder)
        decoder = aud_file_find_decoder(probe_url, false, probe_file);
    if (!decoder) {
        AUDWARN("MSR: probe_remote_length found no decoder for %s\n", (const char *)probe_url);
        return;
    }
    auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
    if (!ip)
        return;
    if (probe_file.fseek(0, VFS_SEEK_SET) != 0) {
        /* proceed anyway if seek fails on non-seekable stream */
    }
    Tuple stream_tuple;
    if (ip->read_tag(probe_url, probe_file, stream_tuple, nullptr)) {
        if (stream_tuple.get_int(Tuple::Length) > 0)
            target_tuple.set_int(Tuple::Length, stream_tuple.get_int(Tuple::Length));
        if (stream_tuple.get_int(Tuple::Bitrate) > 0)
            target_tuple.set_int(Tuple::Bitrate, stream_tuple.get_int(Tuple::Bitrate));
        AUDINFO("MSR: probe_remote_length success for cid %s -> Length: %d ms, Bitrate: %d\n", (const char *)cid, target_tuple.get_int(Tuple::Length), target_tuple.get_int(Tuple::Bitrate));
    }
}

class MsrInputPlugin : public InputPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Monster Siren Input Plugin"),
        PACKAGE,
        nullptr,
        nullptr,
        0
    };

    constexpr MsrInputPlugin()
        : InputPlugin(info, InputInfo(FlagSubtunes)
                                .with_schemes(msr_schemes)
                                .with_priority(1))
    {
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

            // Probe duration fast from disk cache, or probe remotely when preparing for playback (image != nullptr)
            if (tuple.get_int(Tuple::Length) <= 0) {
                MsrSongDetail song = msr_fetch_song_detail(track.cid);
                if (song.duration_ms > 0) {
                    tuple.set_int(Tuple::Length, song.duration_ms);
                } else if (image && song.sourceUrl && song.sourceUrl[0]) {
                    AUDINFO("MSR: Probing remote duration during playback prep for cid %s\n", (const char *)track.cid);
                    probe_remote_length(track.cid, song.sourceUrl, tuple);
                    if (tuple.get_int(Tuple::Length) > 0) {
                        song.duration_ms = tuple.get_int(Tuple::Length);
                        msr_save_song_detail_to_cache(song);
                    }
                } else {
                    String cached_path;
                    if (msr_find_cached_audio(track.cid, cached_path) && song.sourceUrl && song.sourceUrl[0]) {
                        probe_remote_length(track.cid, song.sourceUrl, tuple);
                        if (tuple.get_int(Tuple::Length) > 0) {
                            song.duration_ms = tuple.get_int(Tuple::Length);
                            msr_save_song_detail_to_cache(song);
                        }
                    }
                }
            }

            // Populate lyrics directly from local cache, or fetch over network during playback preparation (image != nullptr)
            if (!tuple.get_str(Tuple::Lyrics)) {
                MsrSongDetail song = msr_fetch_song_detail(track.cid);
                String cached_lyric_path;
                if (msr_find_cached_lyrics(song.lyricUrl, cached_lyric_path)) {
                    AUDINFO("MSR: Loading lyrics from local cache in read_tag for cid %s\n", (const char *)track.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                } else if (image && song.lyricUrl && song.lyricUrl[0]) {
                    AUDINFO("MSR: Fetching remote lyrics during playback prep for cid %s\n", (const char *)track.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                }
                if (image && !tuple.get_str(Tuple::Lyrics))
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

            if (tuple.get_int(Tuple::Length) <= 0) {
                if (song.duration_ms > 0) {
                    tuple.set_int(Tuple::Length, song.duration_ms);
                } else if (image && song.sourceUrl && song.sourceUrl[0]) {
                    AUDINFO("MSR: Probing remote duration during playback prep for standalone song cid %s\n", (const char *)song.cid);
                    probe_remote_length(song.cid, song.sourceUrl, tuple);
                    if (tuple.get_int(Tuple::Length) > 0) {
                        song.duration_ms = tuple.get_int(Tuple::Length);
                        msr_save_song_detail_to_cache(song);
                    }
                } else {
                    String cached_path;
                    if (msr_find_cached_audio(song.cid, cached_path) && song.sourceUrl && song.sourceUrl[0]) {
                        probe_remote_length(song.cid, song.sourceUrl, tuple);
                        if (tuple.get_int(Tuple::Length) > 0) {
                            song.duration_ms = tuple.get_int(Tuple::Length);
                            msr_save_song_detail_to_cache(song);
                        }
                    }
                }
            }

            if (!tuple.get_str(Tuple::Lyrics)) {
                String cached_lyric_path;
                if (msr_find_cached_lyrics(song.lyricUrl, cached_lyric_path)) {
                    AUDINFO("MSR: Loading lyrics from local cache in read_tag for standalone song cid %s\n", (const char *)song.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                } else if (image && song.lyricUrl && song.lyricUrl[0]) {
                    AUDINFO("MSR: Fetching remote lyrics during playback prep for standalone song cid %s\n", (const char *)song.cid);
                    String lyrics = msr_fetch_lyrics(song.lyricUrl);
                    if (lyrics && lyrics[0])
                        tuple.set_str(Tuple::Lyrics, lyrics);
                }
                if (image && !tuple.get_str(Tuple::Lyrics))
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
        if (msr_find_cached_audio(song.cid, cached_path)) {
            AUDINFO("Playing Monster Siren track %s directly from offline disk cache: %s\n", (const char *)song.name, (const char *)cached_path);
            stream_url = String(StringBuf(str_concat({"file://", cached_path})));
        } else if (aud_get_bool("monster-siren", "enable_cache") && aud_get_bool("monster-siren", "cache_audio") && song.sourceUrl) {
            std::thread([cid = String(song.cid), sourceUrl = String(song.sourceUrl)]() {
                msr_fetch_and_cache_audio_background(cid, sourceUrl);
            }).detach();
        }

        AUDINFO("Streaming Monster Siren track %s from URL: %s\n", (const char *)song.name, (const char *)stream_url);

        VFSFile source_file(stream_url, "r");
        PluginHandle * decoder = nullptr;
        if (source_file) {
            decoder = aud_file_find_decoder(stream_url, true, source_file);
            if (!decoder)
                decoder = aud_file_find_decoder(stream_url, false, source_file);
        }

        // If connection or decoder check failed on remote stream (e.g. HTTP 403 CDN token expired),
        // invalidate cached song detail and refetch from API for a brand new sourceUrl + token!
        if ((!source_file || !decoder) && !msr_find_cached_audio(song.cid, cached_path)) {
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

        auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
        if (!ip)
            return false;

        // Ensure playback tuple has duration/bitrate/lyrics populated before starting stream
        Tuple t;
        read_tag(filename, file, t, nullptr);
        if (t.get_int(Tuple::Length) <= 0) {
            if (source_file.fseek(0, VFS_SEEK_SET) != 0) {
                /* proceed anyway if seek fails on non-seekable stream */
            }
            Tuple stream_tuple;
            if (ip->read_tag(stream_url, source_file, stream_tuple, nullptr)) {
                if (stream_tuple.get_int(Tuple::Length) > 0) {
                    t.set_int(Tuple::Length, stream_tuple.get_int(Tuple::Length));
                    if (song.duration_ms <= 0) {
                        song.duration_ms = stream_tuple.get_int(Tuple::Length);
                        msr_save_song_detail_to_cache(song);
                    }
                }
                if (stream_tuple.get_int(Tuple::Bitrate) > 0)
                    t.set_int(Tuple::Bitrate, stream_tuple.get_int(Tuple::Bitrate));
            }
            if (t.get_int(Tuple::Length) <= 0) {
                probe_remote_length(song.cid, stream_url, t);
                if (t.get_int(Tuple::Length) > 0 && song.duration_ms <= 0) {
                    song.duration_ms = t.get_int(Tuple::Length);
                    msr_save_song_detail_to_cache(song);
                }
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
