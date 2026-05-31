#include "common.h"

/* ── Metadata cache ──────────────────────────────────────────────── */

static std::unordered_map<std::string, std::string> s_title_cache;
static std::unordered_map<std::string, std::string> s_artist_cache;
static std::unordered_map<std::string, std::string> s_album_cache;
static std::mutex s_cache_mutex;

void cache_title(const char * id, const char * title)
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_title_cache[id] = title;
}

void cache_artist(const char * id, const char * artist)
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_artist_cache[id] = artist;
}

void cache_album(const char * id, const char * album)
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_album_cache[id] = album;
}

/* Fetch title, artist, and album from yt-dlp in one call. */
static void fetch_metadata(const char * url, const char * id)
{
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        if (s_title_cache.count(id) > 0)
            return;
    }

    AUDINFO("fetch_metadata: fetching title||uploader for %s\n", url);

    if (strchr(url, '\''))
        return;

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "exec yt-dlp --no-playlist "
        "--print 'title||uploader||playlist_title' '%s' 2>/dev/null",
        url);

    FILE * fp = popen(cmd, "r");
    if (!fp)
        return;

    char line[4096];
    if (!fgets(line, sizeof(line), fp))
    {
        pclose(fp);
        return;
    }
    pclose(fp);

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';

    if (line[0] == '\0')
        return;

    char * sep1 = strstr(line, "||");
    if (!sep1)
    {
        cache_title(id, line);
        return;
    }

    *sep1 = '\0';
    const char * title = line;
    char * rest = sep1 + 2;

    char * sep2 = strstr(rest, "||");
    const char * artist;
    const char * album;
    if (sep2)
    {
        *sep2 = '\0';
        artist = rest;
        album = sep2 + 2;
    }
    else
    {
        artist = rest;
        album = "";
    }

    if (title[0])
        cache_title(id, title);
    if (artist[0])
        cache_artist(id, artist);
    if (album[0] && album[0] != 'N' && strcmp(album, "NA") != 0)
        cache_album(id, album);
    else if (artist[0])
        cache_album(id, artist);
}

/* ── Disk cache directory ───────────────────────────────────────── */

const char * cache_dir()
{
    static char dir[4096] = "";
    if (dir[0] == '\0')
    {
        const char * home = getenv("HOME");
        if (!home)
            home = "/tmp";
        snprintf(dir, sizeof(dir), "%s/.cache/audacious-ytdlp", home);
        mkdir(dir, 0755);
    }
    return dir;
}

/* ── URL helpers ────────────────────────────────────────────────── */

String normalize_url(const char * src)
{
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0)
        return String(src);

    char buf[4096];
    snprintf(buf, sizeof(buf), "https://%s", src);
    return String(buf);
}

/* ── Playlist entry updater ────────────────────────────────────── */
/* Called after an entry's metadata has been resolved by yt-dlp to push the
 * title/artist/album into the playlist entry so it shows in the UI immediately
 * rather than waiting for the input plugin's scan (which finds no tags in a raw
 * audio pipe).  Uses remove + re-insert because there is no public
 * Playlist::entry_set_tuple. */

void update_playlist_entry(const char * url, const char * id)
{
    if (!id || !id[0])
        return;

    String title = resolve_title(url, id);
    if (!title || !title[0])
        return;

    Playlist pl = Playlist::active_playlist();
    int n = pl.n_entries();

    for (int i = 0; i < n; i++)
    {
        String fn = pl.entry_filename(i);
        if (!fn || !strstr(fn, id))
            continue;

        /* Skip if title is already set (e.g. by a prior update). */
        Tuple tuple = pl.entry_tuple(i, Playlist::NoWait);
        String existing_title = tuple.get_str(Tuple::Title);
        if (existing_title && existing_title[0])
            break;

        /* COW-copy the existing tuple (preserves any format fields the input
         * plugin may have set) and overlay the yt-dlp metadata. */
        Tuple new_tuple = tuple.ref();
        new_tuple.set_str(Tuple::Title, title);

        String artist = resolve_artist(url, id);
        if (artist && artist[0])
        {
            new_tuple.set_str(Tuple::Artist, artist);

            String album = resolve_album(url, id);
            if (album && album[0])
                new_tuple.set_str(Tuple::Album, album);
        }
        new_tuple.set_state(Tuple::Valid);

        /* Write the tuple back via remove + re-insert. */
        pl.remove_entry(i);
        pl.insert_entry(i, fn, std::move(new_tuple), false);

        AUDINFO("update_playlist_entry: entry %d → %s\n", i, (const char *)title);
        break;
    }
}

const char * extract_video_id(const char * url, char * out, int out_size)
{
    const char * v = strstr(url, "v=");
    if (v)
    {
        v += 2;
        int i = 0;
        while (v[i] && v[i] != '&' && i < out_size - 1)
        {
            out[i] = v[i];
            i++;
        }
        out[i] = '\0';
        return out;
    }

    const char * y = strstr(url, "youtu.be/");
    if (y)
    {
        y += 9;
        int i = 0;
        while (y[i] && y[i] != '?' && y[i] != '/' && i < out_size - 1)
        {
            out[i] = y[i];
            i++;
        }
        out[i] = '\0';
        return out;
    }

    const char * s = strstr(url, "/shorts/");
    if (s)
    {
        s += 8;
        int i = 0;
        while (s[i] && s[i] != '?' && s[i] != '/' && i < out_size - 1)
        {
            out[i] = s[i];
            i++;
        }
        out[i] = '\0';
        return out;
    }

    return nullptr;
}

bool is_youtube_url(const char * url)
{
    return strstr(url, "youtube.com") != nullptr ||
           strstr(url, "youtu.be") != nullptr;
}

bool is_playlist_url(const char * url)
{
    const char * p = strstr(url, "youtube.com/playlist");
    if (!p)
        return false;

    p = strchr(p, '?');
    if (!p)
        return false;

    for (const char * q = p; *q; q++)
    {
        if ((q == p || *(q - 1) == '&') && strncmp(q, "list=", 5) == 0)
            return true;
    }

    return false;
}

void strip_list_param(char * url)
{
    char * p = strstr(url, "&list=");
    if (p)
    {
        char * next = strchr(p + 6, '&');
        if (next)
            memmove(p, next, strlen(next) + 1);
        else
            *p = '\0';
        return;
    }

    p = strchr(url, '?');
    if (!p)
        return;
    p++;
    if (strncmp(p, "list=", 5) != 0)
        return;

    char * next = strchr(p + 5, '&');
    if (next)
        memmove(p - 1, next, strlen(next) + 1);
    else
        *p = '\0';
}

/* ── Metadata resolution (cache + yt-dlp fallback) ──────────────── */

String resolve_title(const char * url, const char * id)
{
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_title_cache.find(id);
        if (it != s_title_cache.end())
            return String(it->second.c_str());
    }

    fetch_metadata(url, id);

    std::lock_guard<std::mutex> lock(s_cache_mutex);
    auto it = s_title_cache.find(id);
    return it != s_title_cache.end() ? String(it->second.c_str()) : String();
}

String resolve_artist(const char * url, const char * id)
{
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_artist_cache.find(id);
        if (it != s_artist_cache.end())
            return String(it->second.c_str());
    }

    fetch_metadata(url, id);

    std::lock_guard<std::mutex> lock(s_cache_mutex);
    auto it = s_artist_cache.find(id);
    return it != s_artist_cache.end() ? String(it->second.c_str()) : String();
}

String resolve_album(const char * url, const char * id)
{
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_album_cache.find(id);
        if (it != s_album_cache.end())
            return String(it->second.c_str());
    }

    fetch_metadata(url, id);

    std::lock_guard<std::mutex> lock(s_cache_mutex);
    auto it = s_album_cache.find(id);
    return it != s_album_cache.end() ? String(it->second.c_str()) : String();
}

/* ── Playlist expansion ─────────────────────────────────────────── */

void expand_playlist(const char * url)
{
    AUDINFO("expand_playlist: url=%s\n", url);

    if (strchr(url, '\''))
    {
        AUDERR("expand_playlist: URL contains single quotes\n");
        return;
    }

    char cmd[8192];
    int n = snprintf(cmd, sizeof(cmd),
        "exec yt-dlp --flat-playlist "
        "--print '%%(id)s||%%(title)s||%%(uploader)s' '%s' 2>/dev/null",
        url);

    if (n >= (int)sizeof(cmd) - 1)
    {
        AUDERR("expand_playlist: URL too long\n");
        return;
    }

    FILE * fp = popen(cmd, "r");
    if (!fp)
    {
        AUDERR("expand_playlist: popen() failed\n");
        return;
    }

    Playlist playlist = Playlist::active_playlist();
    char line[8192];
    int count = 0;

    while (fgets(line, sizeof(line), fp))
    {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (line[0] == '\0')
            continue;

        char * sep1 = strstr(line, "||");
        if (!sep1)
            continue;

        *sep1 = '\0';
        const char * id = line;
        char * rest = sep1 + 2;

    char * sep2 = strstr((char *)rest, "||");
        const char * title;
        const char * artist;
        if (sep2)
        {
            *sep2 = '\0';
            title = rest;
            artist = sep2 + 2;
        }
        else
        {
            title = rest;
            artist = "";
        }

        cache_title(id, title);
        if (artist[0])
            cache_artist(id, artist);
        if (artist[0])
            cache_album(id, artist);

        char video_url[4096];
        snprintf(video_url, sizeof(video_url),
            "ytdlp://www.youtube.com/watch?v=%s", id);

        Tuple tuple;
        tuple.set_str(Tuple::Title, title);
        if (artist[0])
        {
            tuple.set_str(Tuple::Artist, artist);
            tuple.set_str(Tuple::Album, artist);
        }

        playlist.insert_entry(-1, video_url, std::move(tuple), false);
        count++;
    }

    int exit_code = pclose(fp);
    AUDINFO("expand_playlist: added %d entries, yt-dlp exit=%d\n",
            count, exit_code);
}
