#ifndef MONSTER_SIREN_H
#define MONSTER_SIREN_H

#ifndef EXPORT
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
#endif

#include <libaudcore/objects.h>
#include <libaudcore/index.h>
#include <string>
#include <vector>

enum class MsrUrlType {
    Unknown = 0,
    Song,       // e.g. msr://song/515437 or https://monster-siren.hypergryph.com/music/515437
    Album,      // e.g. msr://album/8929 or https://monster-siren.hypergryph.com/info/8929
    AlbumTrack  // e.g. msr://album/8929?1 (track index within album, 1-based)
};

struct MsrUrlParsed {
    MsrUrlType type = MsrUrlType::Unknown;
    String cid;
    int subtune_index = 0; // 1-based index when type is AlbumTrack
};

struct MsrTrack {
    String cid;
    String name;
    String artistes;
};

struct MsrAlbum {
    String cid;
    String name;
    String coverUrl;
    String artistes;
    std::vector<MsrTrack> tracks;
};

struct MsrSongDetail {
    String cid;
    String name;
    String albumCid;
    String sourceUrl;
    String lyricUrl;
    String mvCoverUrl;
    String artists;
    int duration_ms = 0;
};

// URL parsing
MsrUrlParsed msr_parse_url(const char * url);

// API calls (cached where appropriate)
std::vector<MsrAlbum> msr_fetch_albums(bool force_network = false);
MsrAlbum msr_fetch_album_detail(const String & cid);
MsrSongDetail msr_fetch_song_detail(const String & cid);
Index<char> msr_fetch_image(const String & url);
String msr_fetch_lyrics(const String & url);

// Disk caching and offline synchronization API
String msr_get_cache_dir();
bool msr_load_catalog_from_cache(std::vector<MsrAlbum> & out_albums);
void msr_save_catalog_to_cache(const std::vector<MsrAlbum> & albums);
bool msr_find_cached_art(const String & cid_or_url, String & out_path);
void msr_save_art_to_cache(const String & cid_or_url, const Index<char> & data);
bool msr_find_cached_audio(const String & cid, String & out_path);
void msr_save_audio_to_cache(const String & cid, const String & sourceUrl, const Index<char> & data);
bool msr_find_cached_lyrics(const String & cid_or_url, String & out_path);
void msr_save_lyrics_to_cache(const String & cid_or_url, const String & lyrics_text);
void msr_fetch_and_cache_audio_background(const String & cid, const String & sourceUrl);

bool msr_find_cached_album_detail(const String & cid, MsrAlbum & out_album);
void msr_save_album_detail_to_cache(const MsrAlbum & album);
bool msr_find_cached_song_detail(const String & cid, MsrSongDetail & out_song);
void msr_save_song_detail_to_cache(const MsrSongDetail & song);
void msr_invalidate_song_detail(const String & cid);

// Clear memory cache if needed
void msr_cache_clear();

#endif // MONSTER_SIREN_H
