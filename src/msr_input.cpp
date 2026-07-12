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
#include <vector>

static const char * const msr_schemes[] = {"msr", nullptr};

/**
 * Helper function to probe remote audio stream duration over VFS.
 * By querying the underlying audio decoder (e.g. sndfile or ffaudio) before playback,
 * we ensure Tuple::Length is set (> 0). This enables Audacious to display exact track
 * timestamps on the seekbar and permits seeking via HTTP byte-range requests.
 */
static void probe_remote_length(const String & sourceUrl, Tuple & target_tuple)
{
    if (!sourceUrl || !sourceUrl[0])
        return;
    VFSFile probe_file(sourceUrl, "r");
    if (!probe_file)
        return;
    PluginHandle * decoder = aud_file_find_decoder(sourceUrl, false, probe_file);
    if (!decoder)
        decoder = aud_file_find_decoder(sourceUrl, true, probe_file);
    if (!decoder)
        return;
    auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
    if (!ip)
        return;
    Tuple stream_tuple;
    if (ip->read_tag(sourceUrl, probe_file, stream_tuple, nullptr)) {
        if (stream_tuple.get_int(Tuple::Length) > 0)
            target_tuple.set_int(Tuple::Length, stream_tuple.get_int(Tuple::Length));
        if (stream_tuple.get_int(Tuple::Bitrate) > 0)
            target_tuple.set_int(Tuple::Bitrate, stream_tuple.get_int(Tuple::Bitrate));
    }
}

class MsrInputPlugin : public InputPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Monster Siren Input Plugin"),
        PACKAGE
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

            if (image && album.coverUrl && album.coverUrl[0])
                *image = msr_fetch_image(album.coverUrl);

            // Probe exact duration so track shows correct duration and allows seeking right when added
            if (tuple.get_int(Tuple::Length) <= 0) {
                MsrSongDetail song = msr_fetch_song_detail(track.cid);
                if (song.sourceUrl && song.sourceUrl[0])
                    probe_remote_length(song.sourceUrl, tuple);
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

            if (image && coverUrl && coverUrl[0])
                *image = msr_fetch_image(coverUrl);

            if (song.sourceUrl && song.sourceUrl[0] && tuple.get_int(Tuple::Length) <= 0)
                probe_remote_length(song.sourceUrl, tuple);

            return true;
        }

        return false;
    }

    /**
     * Resolves the remote audio stream URL (hycdn.cn) and delegates actual audio
     * decoding/playback to an appropriate underlying decoder plugin.
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

        AUDINFO("Streaming Monster Siren track %s from URL: %s\n", (const char *)song.name, (const char *)song.sourceUrl);

        VFSFile source_file(song.sourceUrl, "r");
        if (!source_file) {
            AUDERR("Could not open VFS connection to %s\n", (const char *)song.sourceUrl);
            return false;
        }

        // Find native audio decoder (such as sndfile or ffaudio) to handle remote audio
        PluginHandle * decoder = aud_file_find_decoder(song.sourceUrl, false, source_file);
        if (!decoder)
            decoder = aud_file_find_decoder(song.sourceUrl, true, source_file);

        if (!decoder) {
            AUDERR("No decoder found for %s\n", (const char *)song.sourceUrl);
            return false;
        }

        auto ip = (InputPlugin *)aud_plugin_get_header(decoder);
        if (!ip)
            return false;

        // Ensure playback tuple has duration/bitrate populated before starting stream
        Tuple t;
        read_tag(filename, file, t, nullptr);
        if (t.get_int(Tuple::Length) <= 0) {
            Tuple stream_tuple;
            if (ip->read_tag(song.sourceUrl, source_file, stream_tuple, nullptr)) {
                if (stream_tuple.get_int(Tuple::Length) > 0)
                    t.set_int(Tuple::Length, stream_tuple.get_int(Tuple::Length));
                if (stream_tuple.get_int(Tuple::Bitrate) > 0)
                    t.set_int(Tuple::Bitrate, stream_tuple.get_int(Tuple::Bitrate));
            }
        }

        if (t.valid())
            set_playback_tuple(std::move(t));

        // Reopen VFS handle cleanly from byte 0 for audio streaming
        source_file = VFSFile(song.sourceUrl, "r");
        if (!source_file) {
            AUDERR("Could not open VFS connection to %s for playback\n", (const char *)song.sourceUrl);
            return false;
        }

        return ip->play(song.sourceUrl, source_file);
    }
};

EXPORT MsrInputPlugin aud_plugin_instance;
