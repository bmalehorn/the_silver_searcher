#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <errno.h>

#include "mmap.h"
#include "log.h"
#include "util.h"

mmap_t *ag_mmap(int fd, off_t f_len) {
    void *buf;

#ifdef _WIN32
    {
        HANDLE hmmap = CreateFileMapping(
            (HANDLE)_get_osfhandle(fd), 0, PAGE_READONLY, 0, f_len, NULL);
        buf = (char *)MapViewOfFile(hmmap, FILE_SHARE_READ, 0, 0, f_len);
        if (hmmap != NULL)
            CloseHandle(hmmap);
    }
    if (buf == NULL) {
        FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, GetLastError(), 0, (void *)&buf, 0, NULL);
        log_err("File failed to load: %s.", buf);
        LocalFree((void *)buf);
        return NULL;
    }
#else
    buf = mmap(0, f_len, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        log_err("File failed to load: %s.", strerror(errno));
        return NULL;
    }
#if HAVE_MADVISE
    madvise(buf, f_len, MADV_SEQUENTIAL);
#elif HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, f_len, POSIX_MADV_SEQUENTIAL);
#endif
#endif

    mmap_t *m = ag_malloc(sizeof(m));
    m->f_len = f_len;
    m->buf = buf;
    return m;
}

void ag_munmap(mmap_t *m) {
#ifdef _WIN32
    UnmapViewOfFile(m->buf);
#else
    munmap(m->buf, m->f_len);
#endif
    free(m);
}
