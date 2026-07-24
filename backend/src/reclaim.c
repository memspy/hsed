#include "reclaim.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hsed_truncate_fd(pid_t pid, int fd, long long *freed_out,
                      char *errbuf, size_t errlen) {
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/%d", (int)pid, fd);

    struct stat st;
    if (stat(fd_path, &st) != 0) {
        snprintf(errbuf, errlen, "could not stat %s: %s", fd_path, strerror(errno));
        return -1;
    }

    /* Opening the magic /proc symlink issues a fresh open(2) against the
     * underlying (deleted) inode. Permission is checked against that
     * inode's current mode/owner — it does NOT inherit whatever access the
     * original process had when it first opened the file. */
    int raw_fd = open(fd_path, O_WRONLY);
    if (raw_fd < 0) {
        snprintf(errbuf, errlen,
                 "could not open %s for writing: %s (need root or the file owner's permission)",
                 fd_path, strerror(errno));
        return -1;
    }

    if (ftruncate(raw_fd, 0) != 0) {
        snprintf(errbuf, errlen, "ftruncate failed on %s: %s", fd_path, strerror(errno));
        close(raw_fd);
        return -1;
    }

    close(raw_fd);
    *freed_out = (long long)st.st_size;
    return 0;
}

int hsed_send_signal(pid_t pid, int sig, char *errbuf, size_t errlen) {
    if (kill(pid, sig) != 0) {
        snprintf(errbuf, errlen, "kill(%d, %d) failed: %s", (int)pid, sig, strerror(errno));
        return -1;
    }
    return 0;
}
