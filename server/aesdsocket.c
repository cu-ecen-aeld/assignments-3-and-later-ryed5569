/**
 * aesdsocket.c
 *
 * - Building from assignment —> Assignment 6 multi-threaded server
 * - Multi-client TCP server on port 9000
 * - Packet = bytes up to and including '\n'
 * - For each packet: append to /var/tmp/aesdsocketdata, then send the entire file back
 * - Timestamp thread appends "timestamp:<RFC2822>\n" every 10 seconds
 * - All file operations are protected by a mutex
 * - Uses singly linked list to manage threads; joins on shutdown
 * - Graceful exit on SIGINT/SIGTERM; -d for daemon mode
*/

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "aesd_ioctl.h"

#define SERVER_PORT "9000"
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define RECV_CHUNK 4096
#define SEND_CHUNK 4096

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define AESD_PATH "/dev/aesdchar"
#else
#define AESD_PATH "/var/tmp/aesdsocketdata"
#endif

static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;

#if !USE_AESD_CHAR_DEVICE
static int g_data_fd = -1;
static pthread_t g_time_tid;
#endif

#if !USE_AESD_CHAR_DEVICE
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct client_thread {
    pthread_t tid;
    int client_fd;
    struct sockaddr_in caddr;
    volatile bool done;
    SLIST_ENTRY(client_thread) entries;
};
static SLIST_HEAD(thread_head, client_thread) g_thread_head = SLIST_HEAD_INITIALIZER(g_thread_head);

// ---------- utility ----------

static void fatal_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    syslog(LOG_ERR, "%s", buf);
}

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_requested = 1;
}

#if !USE_AESD_CHAR_DEVICE
static int open_data_file(void)
{
    // O_APPEND ensures kernel appends are atomic among writers.
    return open(DATAFILE, O_CREAT | O_RDWR | O_APPEND, 0644);
}

static int send_file_contents_locked(int out_fd, int file_fd)
{
    // Precondition: caller holds g_file_mutex
    if (lseek(file_fd, 0, SEEK_SET) == (off_t)-1) return -1;

    char buf[SEND_CHUNK];
    ssize_t r;
    while ((r = read(file_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t s = send(out_fd, buf + off, (size_t)(r - off), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            off += s;
        }
    }
    return (r < 0) ? -1 : 0;
}

static int append_to_file_locked(int file_fd, const char *data, size_t len)
{
    // Precondition: caller holds g_file_mutex; file opened with O_APPEND
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(file_fd, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)w;
    }
    return 0;
}
#endif

static int make_listen_socket(void)
{
    int sfd = -1;
    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       // IPv4 for grading simplicity
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int rc = getaddrinfo(NULL, SERVER_PORT, &hints, &res);
    if (rc != 0) {
        fatal_log("getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;

        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(sfd);
        sfd = -1;
    }
    freeaddrinfo(res);

    if (sfd < 0) {
        fatal_log("bind failed: %s", strerror(errno));
        return -1;
    }
    if (listen(sfd, BACKLOG) != 0) {
        fatal_log("listen failed: %s", strerror(errno));
        close(sfd);
        return -1;
    }
    return sfd;
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    if (chdir("/") != 0) {
        // continue anyway; logging is fine here
        syslog(LOG_WARNING, "chdir('/') failed: %s", strerror(errno));
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        if (nullfd > 2) close(nullfd);
    }
}

// ---------- timestamp thread ----------
#if !USE_AESD_CHAR_DEVICE
static void *timestamp_thread(void *arg)
{
    (void)arg;
    while (!g_exit_requested) {
        // Sleep in 1-second chunks to respond quickly to exit
        for (int i = 0; i < 10 && !g_exit_requested; i++) {
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
        }
        if (g_exit_requested) break;

        time_t now = time(NULL);
        struct tm tminfo;
        localtime_r(&now, &tminfo);

        char tbuf[128];
        // RFC 2822-compatible example: "Mon, 02 Jan 2006 15:04:05 -0700"
        if (strftime(tbuf, sizeof(tbuf), "%a, %d %b %Y %H:%M:%S %z", &tminfo) == 0) {
            continue;
        }

        char line[192];
        int n = snprintf(line, sizeof(line), "timestamp:%s\n", tbuf);
        if (n <= 0) continue;

        pthread_mutex_lock(&g_file_mutex);
        (void)append_to_file_locked(g_data_fd, line, (size_t)n);
        pthread_mutex_unlock(&g_file_mutex);
    }
    return NULL;
}
#endif

// ---------- client thread ----------

struct client_args {
    int cfd;
    struct sockaddr_in caddr;
    struct client_thread *self;
};

static void *handle_client_thread(void *arg)
{
    struct client_args *pargs = (struct client_args *)arg;
    int cfd = pargs->cfd;
    struct sockaddr_in *caddr = &pargs->caddr;
    struct client_thread *self = pargs->self;

    char client_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &caddr->sin_addr, client_ip, sizeof(client_ip));
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    char *line_buf = NULL;
    size_t line_cap = 0;
    size_t line_len = 0;
    char recvbuf[RECV_CHUNK];

    #if USE_AESD_CHAR_DEVICE
        int data_fd = -1;
    #endif

    while (!g_exit_requested) {
        ssize_t n = recv(cfd, recvbuf, sizeof(recvbuf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            // Client closed
            break;
        }

        for (ssize_t i = 0; i < n; i++) {
            // Grow buffer if required
            if (line_len + 1 > line_cap) {
                size_t new_cap = (line_cap == 0) ? 1024 : (line_cap * 2);
                char *tmp = realloc(line_buf, new_cap);
                if (!tmp) {
                    fatal_log("malloc failed; dropping packet fragment");
                    free(line_buf);
                    line_buf = NULL;
                    line_cap = 0;
                    line_len = 0;
                    // Continue scanning until newline resets us
                    continue;
                }
                line_buf = tmp;
                line_cap = new_cap;
            }
            line_buf[line_len++] = recvbuf[i];

            if (recvbuf[i] == '\n') {
            #if USE_AESD_CHAR_DEVICE
                int devfd = open(AESD_PATH, O_RDWR | O_CLOEXEC);
                if (devfd < 0) {
                    fatal_log("open(%s,O_RDWR) failed: %s", AESD_PATH, strerror(errno));
                    goto out;
                }

                while (!g_exit_requested) {
                    ssize_t n = recv(cfd, recvbuf, sizeof(recvbuf), 0);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (n == 0) break; // client closed

                    for (ssize_t i = 0; i < n; i++) {
                        // ensure capacity
                        if (line_len + 1 > line_cap) {
                            size_t new_cap = (line_cap == 0) ? 1024 : (line_cap * 2);
                            char *tmp = realloc(line_buf, new_cap);
                            if (!tmp) {
                                fatal_log("malloc failed; dropping packet fragment");
                                free(line_buf); line_buf = NULL; line_cap = 0; line_len = 0;
                                continue;
                            }
                            line_buf = tmp; line_cap = new_cap;
                        }
                        line_buf[line_len++] = recvbuf[i];

                        if (recvbuf[i] == '\n') {
                            /* NUL-terminate a copy for parsing; do not include NUL in writes */
                            char *z = malloc(line_len + 1);
                            if (!z) { fatal_log("malloc failed"); goto dev_out; }
                            memcpy(z, line_buf, line_len);
                            z[line_len] = '\0';

                            /* Try AESDCHAR_IOCSEEKTO:X,Y */
                            unsigned int X, Y;
                            bool is_cmd = false;
                            if (sscanf(z, "AESDCHAR_IOCSEEKTO:%u,%u", &X, &Y) == 2) {
                                struct aesd_seekto st = { .write_cmd = X, .write_cmd_offset = Y };
                                if (ioctl(devfd, AESDCHAR_IOCSEEKTO, &st) == -1) {
                                    fatal_log("ioctl AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
                                    // fall through to normal write? Spec says do NOT write the command. We skip write.
                                }
                                is_cmd = true;
                            }

                            if (!is_cmd) {
                                // normal line: write to device
                                size_t off = 0;
                                while (off < line_len) {
                                    ssize_t w = write(devfd, line_buf + off, line_len - off);
                                    if (w < 0) { if (errno == EINTR) continue; free(z); goto dev_out; }
                                    off += (size_t)w;
                                }
                            }

                            // read back starting from current f_pos (set by either write or ioctl)
                            for (;;) {
                                char buf[SEND_CHUNK];
                                ssize_t r = read(devfd, buf, sizeof buf);
                                if (r < 0) { if (errno == EINTR) continue; free(z); goto dev_out; }
                                if (r == 0) break;
                                ssize_t off = 0;
                                while (off < r) {
                                    ssize_t s = send(cfd, buf + off, (size_t)(r - off), 0);
                                    if (s < 0) { if (errno == EINTR) continue; free(z); goto dev_out; }
                                    off += s;
                                }
                                if (memchr(buf, '\n', r)) break; // mirror A8 behavior
                            }

                            free(z);
                            line_len = 0; // reset aggregator for next line
                        }
                    }
                }

            dev_out:
                if (devfd >= 0) close(devfd);
            #else
                pthread_mutex_lock(&g_file_mutex);
                if (append_to_file_locked(g_data_fd, line_buf, line_len) != 0) {
                    fatal_log("write failed: %s", strerror(errno));
                    pthread_mutex_unlock(&g_file_mutex);
                    goto out;
                }
                if (send_file_contents_locked(cfd, g_data_fd) != 0) {
                    pthread_mutex_unlock(&g_file_mutex);
                    goto out;
                }
                pthread_mutex_unlock(&g_file_mutex);
                line_len = 0;
            #endif
            }
        }
    }

out:
#if USE_AESD_CHAR_DEVICE
    if (data_fd >= 0) close(data_fd);
#endif
    free(line_buf);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    self->done = true;
    close(cfd);
    free(pargs);
    return NULL;
}

// ---------- main ----------

int main(int argc, char *argv[])
{
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    bool daemon_mode = false;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) daemon_mode = true;

    g_listen_fd = make_listen_socket();
    if (g_listen_fd < 0) {
        closelog();
        return EXIT_FAILURE;
    }

    if (daemon_mode) daemonize();

    #if !USE_AESD_CHAR_DEVICE
    g_data_fd = open_data_file();
    if (g_data_fd < 0) {
        fatal_log("open data file failed: %s", strerror(errno));
        close(g_listen_fd);
        closelog();
        return EXIT_FAILURE;
    }

    // Ensure we start with a clean file for each run
    if (ftruncate(g_data_fd, 0) != 0) {
        fatal_log("ftruncate failed: %s", strerror(errno));
        close(g_listen_fd);
        close(g_data_fd);
        closelog();
        return EXIT_FAILURE;
    }

    if (pthread_create(&g_time_tid, NULL, timestamp_thread, NULL) != 0) {
        fatal_log("timestamp thread create failed");
        close(g_listen_fd);
        close(g_data_fd);
        closelog();
        return EXIT_FAILURE;
    }
    #endif

    // Accept loop
    while (!g_exit_requested) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(g_listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR && g_exit_requested) break;
            if (errno == EINTR) continue;
            fatal_log("accept failed: %s", strerror(errno));
            continue;
        }

        struct client_thread *node = calloc(1, sizeof(*node));
        if (!node) {
            fatal_log("calloc client_thread failed");
            close(cfd);
            continue;
        }
        node->client_fd = cfd;
        node->caddr = caddr;
        node->done = false;

        struct client_args *args = malloc(sizeof(*args));
        if (!args) {
            fatal_log("malloc client_args failed");
            close(cfd);
            free(node);
            continue;
        }
        args->cfd = cfd;
        args->caddr = caddr;
        args->self = node;

        if (pthread_create(&node->tid, NULL, handle_client_thread, args) != 0) {
            fatal_log("pthread_create failed");
            close(cfd);
            free(args);
            free(node);
            continue;
        }

        pthread_mutex_lock(&g_list_mutex);
        SLIST_INSERT_HEAD(&g_thread_head, node, entries);
        pthread_mutex_unlock(&g_list_mutex);

        // Opportunistically reap finished threads using manual SAFE traversal
        pthread_mutex_lock(&g_list_mutex);
        struct client_thread *it = SLIST_FIRST(&g_thread_head);
        struct client_thread *next;
        while (it) {
            next = SLIST_NEXT(it, entries);
            if (it->done) {
                pthread_t tid = it->tid;
                SLIST_REMOVE(&g_thread_head, it, client_thread, entries);
                pthread_mutex_unlock(&g_list_mutex);
                pthread_join(tid, NULL);
                // Client fd closed in thread
                free(it);
                pthread_mutex_lock(&g_list_mutex);
            }
            it = next;
        }
        pthread_mutex_unlock(&g_list_mutex);
    }

    // Shutdown
    syslog(LOG_INFO, "Caught signal, exiting");

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    // Ask all clients to exit and join them
    pthread_mutex_lock(&g_list_mutex);
    struct client_thread *it = SLIST_FIRST(&g_thread_head);
    for (; it; it = SLIST_NEXT(it, entries)) {
        shutdown(it->client_fd, SHUT_RDWR); // unblock recv()
    }
    it = SLIST_FIRST(&g_thread_head);
    struct client_thread *next;
    while (it) {
        next = SLIST_NEXT(it, entries);
        SLIST_REMOVE(&g_thread_head, it, client_thread, entries);
        pthread_mutex_unlock(&g_list_mutex);
        pthread_join(it->tid, NULL);
        close(it->client_fd);
        free(it);
        pthread_mutex_lock(&g_list_mutex);
        it = next;
    }
    SLIST_INIT(&g_thread_head);
    pthread_mutex_unlock(&g_list_mutex);

    #if !USE_AESD_CHAR_DEVICE
    // Stop timestamp thread
    pthread_join(g_time_tid, NULL);

    if (g_data_fd >= 0) {
        close(g_data_fd);
        g_data_fd = -1;
    }

    if (unlink(DATAFILE) != 0 && errno != ENOENT) {
        fatal_log("unlink(%s) failed: %s", DATAFILE, strerror(errno));
    }
    #endif

    closelog();
    return EXIT_SUCCESS;
}
