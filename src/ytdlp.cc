/*
 * ytdlp.cc — Audacious transport plugin for YouTube playback via yt-dlp
 *
 * URI scheme: ytdlp://www.youtube.com/watch?v=...
 *   The plugin strips the ytdlp:// prefix, prepends https://, and spawns
 *   yt-dlp to pipe audio data to Audacious via a pipe(2) + fork(2) + execlp(2)
 *   pipeline.
 *
 * Playlist URLs (youtube.com/playlist?list=...) are expanded in-band: each
 * video ID from --flat-playlist --print id is inserted into the playlist as
 * a ytdlp:// URL.  Single videos with a list= parameter are not expanded.
 *
 * Build: meson setup build && ninja -C build
 * Install: sudo ninja -C build install
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/playlist.h>
#include <libaudcore/runtime.h>

/*
 * Read-only VFS over a yt-dlp pipe.  Audacious opens one YtdlpFile per video
 * URL; the file reads from yt-dlp stdout.  Seeking is limited to forward skip
 * (SEEK_CUR) since pipes don't support random access.
 */
class YtdlpFile : public VFSImpl
{
public:
    YtdlpFile(const char * url);
    ~YtdlpFile() override;

    /* VFSImpl overrides — only reading is supported. */
    int64_t fread(void * ptr, int64_t size, int64_t nmemb) override;
    int fseek(int64_t offset, VFSSeekType whence) override;
    int64_t ftell() override;
    int64_t fsize() override;
    bool feof() override;
    int64_t fwrite(const void *, int64_t, int64_t) override { return -1; }
    int ftruncate(int64_t) override { return -1; }
    int fflush() override { return 0; }

private:
    void start_ytdlp();
    void stop_ytdlp();
    bool skip_bytes(int64_t count);

    String m_url;          /* Full YouTube URL passed to yt-dlp. */
    FILE * m_fp = nullptr; /* Read end of the pipe from yt-dlp. */
    pid_t m_pid = -1;      /* Child PID for cleanup / kill. */
    int64_t m_pos = 0;     /* Logical read position (bytes consumed). */
    bool m_eof = false;    /* Set when the pipe returns EOF. */
};

/*
 * Normalise a URL extracted from the ytdlp:// scheme.
 * If the protocol is already present (http/https) it's returned as-is;
 * otherwise https:// is prepended so yt-dlp receives a well-formed URL.
 */
static String normalize_url(const char * src)
{
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0)
        return String(src);

    char buf[4096];
    snprintf(buf, sizeof(buf), "https://%s", src);
    return String(buf);
}

YtdlpFile::YtdlpFile(const char * url)
{
    AUDINFO("YtdlpFile: opening url=%s\n", url);

    const char * src = url;

    if (strncmp(src, "ytdlp://", 8) == 0)
        src += 8;
    else if (strncmp(src, "ytdlp:", 6) == 0)
        src += 6;

    m_url = normalize_url(src);
    AUDINFO("YtdlpFile: normalized url=%s\n", (const char *)m_url);
    start_ytdlp();
}

YtdlpFile::~YtdlpFile()
{
    stop_ytdlp();
}

/*
 * Spawn yt-dlp in a child process and wire its stdout to a pipe.
 *
 *   pipefd[0] ← child stdout   (parent reads here)
 *   pipefd[1] ← child writes   (closed in parent after fork)
 *
 * yt-dlp is invoked with:
 *   -f bestaudio/best   — prefer audio-only, fall back to any best format
 *   -o -                — write audio data to stdout
 *   --no-playlist       — download only the single video, skip playlist
 *
 * The child's stderr is silenced via /dev/null to avoid polluting the
 * terminal with yt-dlp progress/info messages.
 */
void YtdlpFile::start_ytdlp()
{
    AUDINFO("start_ytdlp: url=%s\n", (const char *)m_url);

    int pipefd[2];

    if (pipe(pipefd) < 0)
    {
        AUDERR("start_ytdlp: pipe() failed\n");
        return;
    }

    m_pid = fork();

    if (m_pid < 0)
    {
        AUDERR("start_ytdlp: fork() failed\n");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (m_pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0)
        {
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        AUDINFO("start_ytdlp: executing yt-dlp\n");
        execlp("yt-dlp", "yt-dlp",
               "-f", "bestaudio/best",
               "-o", "-",
               "--no-playlist",
               (const char *)m_url,
               nullptr);

        AUDERR("start_ytdlp: execlp failed\n");
        _exit(127);
    }

    close(pipefd[1]);

    AUDINFO("start_ytdlp: child pid=%d\n", m_pid);
    m_fp = fdopen(pipefd[0], "r");
    if (!m_fp)
    {
        AUDERR("start_ytdlp: fdopen() failed\n");
        close(pipefd[0]);
        stop_ytdlp();
    }
}

/*
 * Close the pipe and reap the yt-dlp child process.
 * SIGTERM is sent first so the child stops promptly; waitpid with WNOHANG
 * avoids blocking if the child has already exited.
 */
void YtdlpFile::stop_ytdlp()
{
    AUDINFO("stop_ytdlp: pid=%d\n", m_pid);

    if (m_fp)
    {
        fclose(m_fp);
        m_fp = nullptr;
    }

    if (m_pid > 0)
    {
        kill(m_pid, SIGTERM);
        int status;
        waitpid(m_pid, &status, WNOHANG);
        AUDINFO("stop_ytdlp: child reaped, status=%d\n", status);
        m_pid = -1;
    }
}

int64_t YtdlpFile::fread(void * ptr, int64_t size, int64_t nmemb)
{
    if (!m_fp)
    {
        AUDERR("fread: no pipe\n");
        return 0;
    }

    int64_t result = ::fread(ptr, size, nmemb, m_fp);
    AUDINFO("fread: size=%ld nmemb=%ld result=%ld pos=%ld\n",
            (long)size, (long)nmemb, (long)result, (long)m_pos);

    if (result > 0)
        m_pos += result * size;

    if (::feof(m_fp))
    {
        m_eof = true;
        AUDINFO("fread: EOF reached, total pos=%ld\n", (long)m_pos);
    }

    return result;
}

int YtdlpFile::fseek(int64_t offset, VFSSeekType whence)
{
    const char * whence_str = "?";
    if (whence == VFS_SEEK_SET) whence_str = "SET";
    else if (whence == VFS_SEEK_CUR) whence_str = "CUR";
    else if (whence == VFS_SEEK_END) whence_str = "END";

    AUDINFO("fseek: offset=%ld whence=%s pos=%ld\n",
            (long)offset, whence_str, (long)m_pos);

    if (!m_fp)
    {
        AUDERR("fseek: no pipe\n");
        return -1;
    }

    switch (whence)
    {
    case VFS_SEEK_SET:
        AUDINFO("fseek: SEEK_SET not supported\n");
        return -1;

    case VFS_SEEK_CUR:
        if (offset < 0)
        {
            AUDINFO("fseek: backward seek not supported\n");
            return -1;
        }

        return skip_bytes(offset) ? 0 : -1;

    case VFS_SEEK_END:
        AUDINFO("fseek: SEEK_END not supported\n");
        return -1;
    }

    return -1;
}

/*
 * Consume (read + discard) |count| bytes from the pipe.
 * Used to implement forward-seeking (SEEK_CUR).  Returns false on EOF.
 */
bool YtdlpFile::skip_bytes(int64_t count)
{
    AUDINFO("skip_bytes: skipping %ld bytes at pos=%ld\n",
            (long)count, (long)m_pos);

    char buf[4096];

    while (count > 0)
    {
        int64_t chunk = (count > 4096) ? 4096 : count;
        int64_t n = ::fread(buf, 1, chunk, m_fp);

        if (n <= 0)
        {
            m_eof = true;
            AUDINFO("skip_bytes: EOF during skip\n");
            return false;
        }

        m_pos += n;
        count -= n;
    }

    AUDINFO("skip_bytes: done, new pos=%ld\n", (long)m_pos);
    return true;
}

int64_t YtdlpFile::ftell()
{
    AUDINFO("ftell: pos=%ld\n", (long)m_pos);
    return m_pos;
}

int64_t YtdlpFile::fsize()
{
    AUDINFO("fsize: returning -1 (unknown)\n");
    return -1;
}

bool YtdlpFile::feof()
{
    AUDINFO("feof: returning %d\n", m_eof);
    return m_eof;
}

/* Returns true if the URL appears to point to YouTube. */
static bool is_youtube_url(const char * url)
{
    bool result = strstr(url, "youtube.com") != nullptr ||
                  strstr(url, "youtu.be") != nullptr;
    AUDINFO("is_youtube_url: url=%s result=%d\n", url, result);
    return result;
}

/*
 * Returns true if the URL is a YouTube playlist page
 * (youtube.com/playlist?list=...).  Single videos with a list= parameter
 * (watch?v=...&list=...) are not expanded.
 */
static bool is_playlist_url(const char * url)
{
    const char * p = strstr(url, "youtube.com/playlist");
    AUDINFO("is_playlist_url: url=%s, found_playlist_path=%d\n", url, p != nullptr);

    if (!p)
        return false;

    p = strchr(p, '?');
    if (!p)
    {
        AUDINFO("is_playlist_url: no query string\n");
        return false;
    }

    for (const char * q = p; *q; q++)
    {
        if ((q == p || *(q - 1) == '&') && strncmp(q, "list=", 5) == 0)
        {
            AUDINFO("is_playlist_url: found list= parameter\n");
            return true;
        }
    }

    AUDINFO("is_playlist_url: no list= parameter\n");
    return false;
}

/*
 * Resolve a YouTube playlist URL into individual video entries.
 *
 * Runs yt-dlp --flat-playlist --print id to get all video IDs without
 * downloading any media, then inserts each as a ytdlp:// URL into the
 * active Audacious playlist.  The transport plugin's own fopen() is
 * triggered for each entry later.
 */
static void expand_playlist(const char * url)
{
    AUDINFO("expand_playlist: url=%s\n", url);

    if (strchr(url, '\''))
    {
        AUDERR("expand_playlist: URL contains single quotes\n");
        return;
    }

    char cmd[4096];
    int n = snprintf(cmd, sizeof(cmd),
        "exec yt-dlp --flat-playlist --print id '%s' 2>/dev/null", url);

    if (n >= (int)sizeof(cmd) - 1)
    {
        AUDERR("expand_playlist: URL too long\n");
        return;
    }

    AUDINFO("expand_playlist: running: %s\n", cmd);
    FILE * fp = popen(cmd, "r");
    if (!fp)
    {
        AUDERR("expand_playlist: popen() failed\n");
        return;
    }

    Playlist playlist = Playlist::active_playlist();
    char id[256];
    int count = 0;

    while (fgets(id, sizeof(id), fp))
    {
        size_t len = strlen(id);
        if (len > 0 && id[len - 1] == '\n')
            id[len - 1] = '\0';

        if (id[0] == '\0')
            continue;

        char video_url[4096];
        snprintf(video_url, sizeof(video_url),
            "ytdlp://www.youtube.com/watch?v=%s", id);

        AUDINFO("expand_playlist: adding entry %d: %s\n", count + 1, video_url);
        playlist.insert_entry(-1, video_url, Tuple(), false);
        count++;
    }

    int exit_code = pclose(fp);
    AUDINFO("expand_playlist: added %d entries, yt-dlp exit=%d\n",
            count, exit_code);
}

/*
 * Audacious transport plugin for the "ytdlp" URI scheme.
 *
 * Registered schemes:
 *   ytdlp  — ytdlp://www.youtube.com/watch?v=...
 *            ytdlp://www.youtube.com/playlist?list=...
 *
 * The plugin also accepts ytdlp: without the double slash for
 * backwards compatibility.
 */
class YtdlpTransport : public TransportPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("yt-dlp Transport"),
        PACKAGE,
        nullptr,
        nullptr,
        0
    };

    static constexpr const char * schemes[] = {"ytdlp"};

    constexpr YtdlpTransport() : TransportPlugin(info, schemes) {}

    VFSImpl * fopen(const char * filename, const char * mode,
                    String & error) override;
};

VFSImpl * YtdlpTransport::fopen(const char * filename, const char * mode,
                                String & error)
{
    AUDINFO("fopen: filename=%s mode=%s\n", filename, mode);

    if (strcmp(mode, "r") && strcmp(mode, "rb"))
    {
        AUDERR("fopen: unsupported mode '%s'\n", mode);
        error = String("unsupported mode");
        return nullptr;
    }

    const char * url = filename;

    if (strncmp(url, "ytdlp://", 8) == 0)
    {
        AUDINFO("fopen: stripping ytdlp:// prefix\n");
        url += 8;
    }
    else if (strncmp(url, "ytdlp:", 6) == 0)
    {
        AUDINFO("fopen: stripping ytdlp: prefix\n");
        url += 6;
    }
    else if (!is_youtube_url(url))
    {
        AUDINFO("fopen: not a YouTube URL, passing through\n");
        return nullptr;
    }

    String normalized = normalize_url(url);
    AUDINFO("fopen: normalized url=%s\n", (const char *)normalized);

    if (is_playlist_url(filename))
    {
        AUDINFO("fopen: detected playlist URL, expanding\n");
        expand_playlist((const char *)normalized);
        return nullptr;
    }

    AUDINFO("fopen: creating YtdlpFile\n");
    return new YtdlpFile(url);
}

/* Required entry point — Audacious scans *.so in Transport/ for this symbol.
   The visibility attribute exports it even with -fvisibility=hidden. */
__attribute__((visibility("default"))) YtdlpTransport aud_plugin_instance;
