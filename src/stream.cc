#include "common.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

/* ═══════════════════════════════════════════════════════════════════
 *  CachedFile — local-file VFSImpl (full seeking, known size)
 * ═══════════════════════════════════════════════════════════════════ */

CachedFile::CachedFile(const char * path, const char * url)
    : m_url(String(url))
{
    m_fp = fopen(path, "rb");
    if (!m_fp)
    {
        AUDERR("CachedFile: fopen(%s) failed\n", path);
        return;
    }

    fseeko(m_fp, 0, SEEK_END);
    m_size = ftello(m_fp);
    fseeko(m_fp, 0, SEEK_SET);
    AUDINFO("CachedFile: %s size=%ld\n", path, (long)m_size);
}

CachedFile::~CachedFile()
{
    if (m_fp) fclose(m_fp);
}

int64_t CachedFile::fread(void * ptr, int64_t size, int64_t nmemb)
{
    return m_fp ? ::fread(ptr, size, nmemb, m_fp) : 0;
}

int CachedFile::fseek(int64_t offset, VFSSeekType whence)
{
    if (!m_fp) return -1;

    static const int map[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    int std_whence = (whence >= 0 && whence < 3) ? map[whence] : SEEK_SET;
    return ::fseeko(m_fp, offset, std_whence);
}

int64_t CachedFile::ftell()  { return m_fp ? ::ftello(m_fp) : 0; }
int64_t CachedFile::fsize()  { return m_size; }
bool CachedFile::feof()      { return m_fp ? ::feof(m_fp) : true; }

String CachedFile::get_metadata(const char * field)
{
    char id[128];
    if (!extract_video_id((const char *)m_url, id, sizeof(id)))
        return String();

    if (strcmp(field, "title") == 0)
        return resolve_title((const char *)m_url, id);

    if (strcmp(field, "artist") == 0)
        return resolve_artist((const char *)m_url, id);

    if (strcmp(field, "album") == 0)
        return resolve_album((const char *)m_url, id);

    return String();
}

/* ═══════════════════════════════════════════════════════════════════
 *  YtdlpFile — pipe-based VFSImpl  (fallback when cache is unavailable)
 * ═══════════════════════════════════════════════════════════════════ */

YtdlpFile::YtdlpFile(const char * url)
{
    const char * src = url;

    if (strncmp(src, "ytdlp://", 8) == 0)
        src += 8;
    else if (strncmp(src, "ytdlp:", 6) == 0)
        src += 6;

    m_url = normalize_url(src);
    AUDINFO("YtdlpFile: url=%s\n", (const char *)m_url);
    start_ytdlp();
}

YtdlpFile::~YtdlpFile()
{
    stop_ytdlp();
}

void YtdlpFile::start_ytdlp()
{
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

    m_fp = fdopen(pipefd[0], "r");
    if (!m_fp)
    {
        AUDERR("start_ytdlp: fdopen() failed\n");
        close(pipefd[0]);
        stop_ytdlp();
    }
}

void YtdlpFile::stop_ytdlp()
{
    if (m_fp)
    {
        fclose(m_fp);
        m_fp = nullptr;
    }

    if (m_pid > 0)
    {
        kill(m_pid, SIGTERM);
        waitpid(m_pid, nullptr, WNOHANG);
        m_pid = -1;
    }
}

int64_t YtdlpFile::fread(void * ptr, int64_t size, int64_t nmemb)
{
    if (!m_fp) return 0;

    int64_t result = ::fread(ptr, size, nmemb, m_fp);

    if (result > 0)
        m_pos += result * size;

    if (::feof(m_fp))
        m_eof = true;

    return result;
}

int YtdlpFile::fseek(int64_t offset, VFSSeekType whence)
{
    if (!m_fp) return -1;

    switch (whence)
    {
    case VFS_SEEK_SET:
        return -1;
    case VFS_SEEK_CUR:
        if (offset < 0) return -1;
        return skip_bytes(offset) ? 0 : -1;
    case VFS_SEEK_END:
        return -1;
    }

    return -1;
}

bool YtdlpFile::skip_bytes(int64_t count)
{
    char buf[4096];

    while (count > 0)
    {
        int64_t chunk = (count > 4096) ? 4096 : count;
        int64_t n = ::fread(buf, 1, chunk, m_fp);

        if (n <= 0)
        {
            m_eof = true;
            return false;
        }

        m_pos += n;
        count -= n;
    }

    return true;
}

int64_t YtdlpFile::ftell()  { return m_pos; }
int64_t YtdlpFile::fsize()  { return -1; }
bool YtdlpFile::feof()      { return m_eof; }

String YtdlpFile::get_metadata(const char * field)
{
    char id[128];
    if (!extract_video_id((const char *)m_url, id, sizeof(id)))
        return String();

    if (strcmp(field, "title") == 0)
        return resolve_title((const char *)m_url, id);

    if (strcmp(field, "artist") == 0)
        return resolve_artist((const char *)m_url, id);

    if (strcmp(field, "album") == 0)
        return resolve_album((const char *)m_url, id);

    return String();
}
