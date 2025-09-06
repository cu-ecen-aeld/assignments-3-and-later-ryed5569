/**
 * aesdsocket.c
 *
 * Single-threaded line-oriented TCP server:
 * - Binds to port 9000 (INADDR_ANY).
 * - Accepts one client at a time.
 * - For each newline-terminated packet received:
 *     - Appends the packet (including the '\n') to /var/tmp/aesdsocketdata
 *     - Immediately sends the full contents of /var/tmp/aesdsocketdata back to the client
 * - Logs "Accepted connection from XXX" and "Closed connection from XXX" to syslog.
 * - Runs forever until SIGINT or SIGTERM, then logs "Caught signal, exiting", closes sockets,
 *   and deletes /var/tmp/aesdsocketdata.
 * - Supports -d to daemonize after verifying bind succeeds.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define SERVER_PORT "9000"
#define DATAFILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define RECV_CHUNK 4096
#define SEND_CHUNK 4096

static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_data_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_requested = 1;
    // Intentionally do not close here. Let main loop handle cleanup.
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    // Do NOT set SA_RESTART: we want accept() to return EINTR on signal.
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

static int open_data_file(void)
{
    // Open once for the process lifetime. Use O_APPEND semantics for writes and O_RDWR for later lseek+read.
    int fd = open(DATAFILE, O_CREAT | O_RDWR, 0644);
    return fd;
}

static int send_file_contents(int out_fd, int file_fd)
{
    if (lseek(file_fd, 0, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    char buf[SEND_CHUNK];
    ssize_t r;
    while ((r = read(file_fd, buf, sizeof(buf))) > 0) {
        ssize_t sent = 0;
        while (sent < r) {
            ssize_t s = send(out_fd, buf + sent, (size_t)(r - sent), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            sent += s;
        }
    }
    return (r < 0) ? -1 : 0;
}

static int append_to_file(int file_fd, const char *data, size_t len)
{
    // Use a simple write loop. File opened O_RDWR; we rely on explicit lseek for reading.
    ssize_t written = 0;
    while ((size_t)written < len) {
        ssize_t w = write(file_fd, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += w;
    }
    return 0;
}

static int make_listen_socket(void)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int sfd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // 0.0.0.0
    hints.ai_protocol = 0;

    rc = getaddrinfo(NULL, SERVER_PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }

    sfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sfd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    int optval = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) != 0) {
        syslog(LOG_ERR, "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(sfd);
        freeaddrinfo(res);
        return -1;
    }

    if (bind(sfd, res->ai_addr, res->ai_addrlen) != 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(sfd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    if (listen(sfd, BACKLOG) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sfd);
        return -1;
    }

    return sfd;
}

static int daemonize_process(void)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) {
        // Parent exits
        exit(EXIT_SUCCESS);
    }
    if (setsid() < 0) return -1;

    pid = fork(); // second fork prevents reacquiring a controlling terminal
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    if (chdir("/") != 0) return -1;

    // Redirect stdio to /dev/null
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        if (nullfd > 2) close(nullfd);
    }
    return 0;
}

static void handle_client(int cfd, struct sockaddr_in *caddr)
{
    char client_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &caddr->sin_addr, client_ip, sizeof(client_ip));
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    // Buffer to accumulate data until a newline is seen.
    char *line_buf = NULL;
    size_t line_cap = 0;
    size_t line_len = 0;

    char recvbuf[RECV_CHUNK];

    // Receive loop
    while (!g_exit_requested) {
        ssize_t n = recv(cfd, recvbuf, sizeof(recvbuf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            // Socket error; abort this client.
            break;
        }
        if (n == 0) {
            // Client closed connection
            break;
        }

        // Process received bytes; extract newline-terminated packets.
        for (ssize_t i = 0; i < n; i++) {
            // Append one byte to line buffer
            if (line_len + 1 > line_cap) {
                size_t new_cap = (line_cap == 0) ? 1024 : (line_cap * 2);
                char *tmp = realloc(line_buf, new_cap);
                if (!tmp) {
                    // On malloc failure, log and drop over-length packet per spec.
                    syslog(LOG_ERR, "malloc failed; discarding packet fragment");
                    line_len = 0;
                    line_cap = 0;
                    free(line_buf);
                    line_buf = NULL;
                    // Continue scanning bytes but ignore until newline
                } else {
                    line_buf = tmp;
                    line_cap = new_cap;
                }
            }
            if (line_cap > 0) {
                line_buf[line_len++] = recvbuf[i];
            }

            if (recvbuf[i] == '\n') {
                // Complete packet available in line_buf[0..line_len-1]
                if (g_data_fd >= 0 && line_cap > 0) {
                    if (append_to_file(g_data_fd, line_buf, line_len) != 0) {
                        syslog(LOG_ERR, "write to data file failed: %s", strerror(errno));
                        // Keep serving but do not crash.
                    } else {
                        // After appending, send the entire file back.
                        if (send_file_contents(cfd, g_data_fd) != 0) {
                            // If send fails, terminate client loop.
                            goto client_done;
                        }
                    }
                }
                // Reset for next packet
                line_len = 0;
            }
        }
    }

client_done:
    free(line_buf);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;

    // Simple argument parsing
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc > 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return EXIT_FAILURE;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (install_signal_handlers() != 0) {
        syslog(LOG_ERR, "Failed to install signal handlers: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    g_listen_fd = make_listen_socket();
    if (g_listen_fd < 0) {
        // make_listen_socket() already syslogged details
        closelog();
        return EXIT_FAILURE; // Return -1 semantics mapped to failure exit
    }

    if (daemon_mode) {
        if (daemonize_process() != 0) {
            syslog(LOG_ERR, "Failed to daemonize: %s", strerror(errno));
            close(g_listen_fd);
            closelog();
            return EXIT_FAILURE;
        }
    }

    // Open data file once
    g_data_fd = open_data_file();
    if (g_data_fd < 0) {
        syslog(LOG_ERR, "Failed to open data file: %s", strerror(errno));
        close(g_listen_fd);
        closelog();
        return EXIT_FAILURE;
    }

    // Accept loop
    while (!g_exit_requested) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        g_client_fd = accept(g_listen_fd, (struct sockaddr *)&caddr, &clen);
        if (g_client_fd < 0) {
            if (errno == EINTR && g_exit_requested) {
                // interrupted by signal, exit gracefully
                break;
            }
            // Other transient error
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        handle_client(g_client_fd, &caddr);
        close(g_client_fd);
        g_client_fd = -1;
    }

    // Graceful shutdown
    syslog(LOG_INFO, "Caught signal, exiting");

    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_data_fd >= 0) {
        close(g_data_fd);
        g_data_fd = -1;
    }

    // Remove data file as required
    if (unlink(DATAFILE) != 0) {
        // Not fatal, but log it
        if (errno != ENOENT)
            syslog(LOG_ERR, "unlink(%s) failed: %s", DATAFILE, strerror(errno));
    }

    closelog();
    return EXIT_SUCCESS;
}
