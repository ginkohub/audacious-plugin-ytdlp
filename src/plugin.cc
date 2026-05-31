#include "common.h"
#include <libaudcore/hook.h>
#include <libaudcore/preferences.h>
#include <set>
#include <thread>

/* ── Playlist update hook ─────────────────────────────────────────── */
/*
 * Called when the playlist changes.  We hook Structure (entry added) and
 * Metadata (scan complete) events to push yt-dlp metadata into the playlist
 * entry's tuple, since the input plugin cannot discover title/artist from a
 * raw audio pipe.
 */

static void playlist_update_cb(void *, void *)
{
    auto detail = Playlist::active_playlist().update_detail();
    if (detail.level < Playlist::Structure)
        return;

    Playlist pl = Playlist::active_playlist();
    int start = detail.before;
    int end = pl.n_entries() - detail.after;

    for (int i = start; i < end; i++)
    {
        String fn = pl.entry_filename(i);
        if (!fn)
            continue;

        const char * url = fn;
        if (strncmp(url, "ytdlp://", 8) == 0)
            url += 8;
        else if (strncmp(url, "ytdlp:", 6) == 0)
            url += 6;
        else
            continue;

        char id[128];
        String normalized = normalize_url(url);
        if (!extract_video_id(normalized, id, sizeof(id)))
            continue;

        update_playlist_entry(normalized, id);
    }
}

/* ── Config defaults ─────────────────────────────────────────────── */

static const char * const defaults[] = {
    "cache", "TRUE",
    nullptr
};

static void ensure_defaults()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    aud_config_set_defaults("ytdlp", defaults);
}

/* ── Plugin preferences ─────────────────────────────────────────── */

static constexpr PreferencesWidget prefs_widgets[] = {
    WidgetCheck(N_("Cache downloaded videos"),
                WidgetBool("ytdlp", "cache"))
};

static constexpr PluginPreferences plugin_prefs = { prefs_widgets };

/* ── PluginInfo and constructor ─────────────────────────────────── */

const PluginInfo YtdlpTransport::info = {
    N_("yt-dlp Transport"),
    PACKAGE,
    nullptr,
    &plugin_prefs,
    0
};

YtdlpTransport::YtdlpTransport() : TransportPlugin(info, schemes) {}

bool YtdlpTransport::init()
{
    hook_associate("playlist update", playlist_update_cb, nullptr);
    return true;
}

void YtdlpTransport::cleanup()
{
    hook_dissociate("playlist update", playlist_update_cb, nullptr);
}

/*
 * Try to find a cached copy of |id| in the disk cache.
 * Returns the full path in |out| if found, nullptr otherwise.
 */
static const char * cache_lookup(const char * id, char * out, int out_size)
{
    const char * dir = cache_dir();
    const char * exts[] = {".m4a", ".webm", ".opus", ".mkv", ".mp3", ""};

    for (int i = 0; exts[i][0]; i++)
    {
        snprintf(out, out_size, "%s/%s%s", dir, id, exts[i]);
        if (access(out, F_OK) == 0)
            return out;
    }

    return nullptr;
}

/*
 * Download a YouTube video to the disk cache.
 * Returns the path to the cached file in |out|, or nullptr on failure.
 */
static const char * download_to_cache(const char * url, const char * id,
                                      char * out, int out_size)
{
    const char * dir = cache_dir();

    /* Step 1: get the file extension from yt-dlp. */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "exec yt-dlp --no-playlist --print ext -f bestaudio '%s' 2>/dev/null", url);

    FILE * fp = popen(cmd, "r");
    if (!fp)
        return nullptr;

    char ext[64];
    if (!fgets(ext, sizeof(ext), fp))
    {
        pclose(fp);
        return nullptr;
    }
    pclose(fp);

    size_t elen = strlen(ext);
    if (elen > 0 && ext[elen - 1] == '\n')
        ext[elen - 1] = '\0';

    if (ext[0] == '\0')
        return nullptr;

    /* Step 2: download to a temp file, then rename to final path. */
    char tmp_path[8192];
    char final_path[8192];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", dir, id);
    snprintf(final_path, sizeof(final_path), "%s/%s.%s", dir, id, ext);

    char dl_cmd[16384];
    snprintf(dl_cmd, sizeof(dl_cmd),
        "exec yt-dlp --no-playlist -f bestaudio --embed-metadata "
        "-o '%s' '%s' 2>/dev/null", tmp_path, url);

    snprintf(out, out_size, "%s", final_path);

    system(dl_cmd);

    if (access(tmp_path, F_OK) != 0)
    {
        AUDERR("download_to_cache: yt-dlp did not produce %s\n", id);
        return nullptr;
    }

    rename(tmp_path, out);
    AUDINFO("download_to_cache: %s -> %s\n", id, out);
    return out;
}

/* ── Background download manager ──────────────────────────────────── */

static std::set<std::string> s_downloading;
static std::mutex s_dl_mutex;

/* Atomically claim this ID for download. */
static bool claim_download(const char * id)
{
    std::lock_guard<std::mutex> lock(s_dl_mutex);
    return s_downloading.insert(id).second;
}

static void mark_done(const char * id)
{
    std::lock_guard<std::mutex> lock(s_dl_mutex);
    s_downloading.erase(id);
}

struct DlJob
{
    std::string url;
    std::string id;
};

static void bg_thread(DlJob job)
{
    char cache_path[8192];
    const char * result = download_to_cache(
        job.url.c_str(), job.id.c_str(), cache_path, sizeof(cache_path));
    if (result)
        AUDINFO("bg_thread: cached %s\n", result);
    else
        AUDERR("bg_thread: failed to cache %s\n", job.id.c_str());

    update_playlist_entry(job.url.c_str(), job.id.c_str());
    mark_done(job.id.c_str());
}

/* ── TransportPlugin::fopen ─────────────────────────────────────── */

VFSImpl * YtdlpTransport::fopen(const char * filename, const char * mode,
                                String & error)
{
    ensure_defaults();

    if (strcmp(mode, "r") && strcmp(mode, "rb"))
    {
        error = String("unsupported mode");
        return nullptr;
    }

    const char * url = filename;

    if (strncmp(url, "ytdlp://", 8) == 0)
        url += 8;
    else if (strncmp(url, "ytdlp:https://", 14) == 0)
        url += 14;
    else if (strncmp(url, "ytdlp:http://", 13) == 0)
        url += 13;
    else if (strncmp(url, "ytdlp:", 6) == 0)
        url += 6;
    else if (!is_youtube_url(url))
        return nullptr;

    String normalized = normalize_url(url);

    /* Playlist URLs are expanded here; no VFSImpl returned. */
    if (is_playlist_url(filename))
    {
        expand_playlist((const char *)normalized);
        return nullptr;
    }

    /* Strip list= so yt-dlp never sees the playlist context. */
    char clean_url[4096];
    snprintf(clean_url, sizeof(clean_url), "%s", (const char *)normalized);
    strip_list_param(clean_url);

    /* Pre-cache the title for get_metadata(). */
    char id[128];
    char cache_path[8192];

    if (extract_video_id(clean_url, id, sizeof(id)))
    {
        resolve_title(clean_url, id);

        if (aud_get_bool("ytdlp", "cache"))
        {
            /* Cache hit — play from disk. */
            if (cache_lookup(id, cache_path, sizeof(cache_path)))
            {
                AUDINFO("fopen: cache hit %s\n", cache_path);
                return new CachedFile(cache_path, clean_url);
            }

            /* Cache miss — start background download, return pipe. */
            if (claim_download(id))
            {
                AUDINFO("fopen: background download %s\n", id);
                std::thread t(bg_thread,
                    DlJob{clean_url, id});
                t.detach();
            }
            else
            {
                AUDINFO("fopen: already downloading %s\n", id);
            }
        }
    }

    return new YtdlpFile(clean_url);
}

/* Required entry point for Audacious plugin loading. */
__attribute__((visibility("default"))) YtdlpTransport aud_plugin_instance;
