// finder-app/writer.c

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    // Use the LOG_USER facility and include PID in messages
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Usage: %s <writefile> <writestr>", argv[0]);
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    // Required debug message
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    // Open the file; overwrite if it exists. Caller is responsible for directory creation.
    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        syslog(LOG_ERR, "Error opening %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    size_t len = strlen(writestr);
    ssize_t written_total = 0;

    while ((size_t)written_total < len) {
        ssize_t n = write(fd, writestr + written_total, len - (size_t)written_total);
        if (n < 0) {
            syslog(LOG_ERR, "Error writing to %s: %s", writefile, strerror(errno));
            close(fd);  // best-effort close
            closelog();
            return 1;
        }
        written_total += n;
    }

    if (close(fd) != 0) {
        syslog(LOG_ERR, "Error closing %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
