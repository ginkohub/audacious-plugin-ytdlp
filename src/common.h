#ifndef YTDLP_COMMON_H
#define YTDLP_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/playlist.h>
#include <libaudcore/runtime.h>

/* ── URL helpers ───────────────────────────────────────────────── */

String normalize_url(const char * src);
const char * extract_video_id(const char * url, char * out, int out_size);
bool is_youtube_url(const char * url);
bool is_playlist_url(const char * url);
void strip_list_param(char * url);

/* ── Metadata cache ──────────────────────────────────────────────── */

String resolve_title(const char * url, const char * id);
String resolve_artist(const char * url, const char * id);
String resolve_album(const char * url, const char * id);
void cache_title(const char * id, const char * title);
void cache_artist(const char * id, const char * artist);
void cache_album(const char * id, const char * album);

/* ── Disk cache ─────────────────────────────────────────────────── */

const char * cache_dir();

/* ── VFS stream classes ─────────────────────────────────────────── */

class CachedFile : public VFSImpl
{
public:
    CachedFile(const char * path, const char * url);
    ~CachedFile() override;

    int64_t fread(void * ptr, int64_t size, int64_t nmemb) override;
    int fseek(int64_t offset, VFSSeekType whence) override;
    int64_t ftell() override;
    int64_t fsize() override;
    bool feof() override;
    int64_t fwrite(const void *, int64_t, int64_t) override { return -1; }
    int ftruncate(int64_t) override { return -1; }
    int fflush() override { return 0; }
    String get_metadata(const char * field) override;

private:
    String m_url;
    FILE * m_fp = nullptr;
    int64_t m_size = 0;
};

class YtdlpFile : public VFSImpl
{
public:
    YtdlpFile(const char * url);
    ~YtdlpFile() override;

    int64_t fread(void * ptr, int64_t size, int64_t nmemb) override;
    int fseek(int64_t offset, VFSSeekType whence) override;
    int64_t ftell() override;
    int64_t fsize() override;
    bool feof() override;
    int64_t fwrite(const void *, int64_t, int64_t) override { return -1; }
    int ftruncate(int64_t) override { return -1; }
    int fflush() override { return 0; }
    String get_metadata(const char * field) override;

private:
    void start_ytdlp();
    void stop_ytdlp();
    bool skip_bytes(int64_t count);

    String m_url;
    FILE * m_fp = nullptr;
    pid_t m_pid = -1;
    int64_t m_pos = 0;
    bool m_eof = false;
};

/* ── Playlist helpers ────────────────────────────────────────────── */

void expand_playlist(const char * url);
void update_playlist_entry(const char * url, const char * id);

/* ── Transport plugin ───────────────────────────────────────────── */

class YtdlpTransport : public TransportPlugin
{
public:
    static const PluginInfo info;
    static constexpr const char * schemes[] = {"ytdlp", "ytdlp:https", "ytdlp:http"};

    YtdlpTransport();

    bool init() override;
    void cleanup() override;

    VFSImpl * fopen(const char * filename, const char * mode,
                    String & error) override;
};

#endif
