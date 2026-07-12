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
};

// URL parsing
MsrUrlParsed msr_parse_url(const char * url);

// API calls (cached where appropriate)
std::vector<MsrAlbum> msr_fetch_albums();
MsrAlbum msr_fetch_album_detail(const String & cid);
MsrSongDetail msr_fetch_song_detail(const String & cid);
Index<char> msr_fetch_image(const String & url);

// Clear memory cache if needed
void msr_cache_clear();

#endif // MONSTER_SIREN_H
