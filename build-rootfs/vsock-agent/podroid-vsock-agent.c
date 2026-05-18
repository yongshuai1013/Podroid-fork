/*
 * Podroid - Rootless Podman for Android
 * Copyright (C) 2024-2026 Podroid contributors
 *
 * podroid-vsock-agent — guest-side peer for Podroid's AVF backend.
 *
 * Reads /etc/podroid/forwards.conf at startup. Each line is one of:
 *
 *   <vport> tcp <host> <gport>     listen on AF_VSOCK port vport, splice each
 *                                  accepted connection to TCP host:gport
 *   <vport> ctl                    listen on AF_VSOCK port vport, accept ONE
 *                                  control connection at a time, handle the
 *                                  line-oriented protocol below
 *
 * Control protocol (LF-terminated ASCII):
 *
 *   RESIZE <rows> <cols>           stty rows/cols on /dev/ttyS0, also persist
 *                                  to /run/term_size for podroid-login restore
 *   ADD    <vport> tcp <host> <gp> append to forwards.conf, fork new TCP
 *                                  listener immediately (idempotent on vport)
 *   REMOVE <vport>                 kill listener child for vport, remove line
 *   PING                           reply "PONG\n"
 *
 * Listener children are tracked by {vport, pid} in a small in-memory table so
 * REMOVE can SIGTERM the correct child. The control loop runs in the parent
 * so the table is the parent's local state — no IPC needed.
 *
 * All TCP listeners bind to AF_VSOCK with CID = VMADDR_CID_ANY (host can reach
 * us regardless of the assigned CID).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CONFIG_PATH     "/etc/podroid/forwards.conf"
#define TERM_SIZE_PATH  "/run/term_size"
#define LOG_TAG         "podroid-vsock-agent"

/* ── Logging ─────────────────────────────────────────────────────────────── */

static void logmsg(const char *level, const char *fmt, ...) {
    fprintf(stderr, "%s [%s] ", LOG_TAG, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

#define LOG_I(...) logmsg("info",  __VA_ARGS__)
#define LOG_W(...) logmsg("warn",  __VA_ARGS__)
#define LOG_E(...) logmsg("error", __VA_ARGS__)

/* ── Listener table ─────────────────────────────────────────────────────── */

#define MAX_LISTENERS 64
struct listener { int vport; pid_t pid; };
static struct listener listeners[MAX_LISTENERS];
static int listener_count = 0;

static int listener_add(int vport, pid_t pid) {
    if (listener_count >= MAX_LISTENERS) return -1;
    listeners[listener_count].vport = vport;
    listeners[listener_count].pid = pid;
    listener_count++;
    return 0;
}

static int listener_find(int vport) {
    for (int i = 0; i < listener_count; i++)
        if (listeners[i].vport == vport) return i;
    return -1;
}

static void listener_remove(int idx) {
    if (idx < 0 || idx >= listener_count) return;
    listeners[idx] = listeners[listener_count - 1];
    listener_count--;
}

/* ── Splice loop (TCP listener child) ───────────────────────────────────── */

static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        p += w; left -= w;
    }
    return (ssize_t)n;
}

static void splice_loop(int a, int b) {
    char buf[16384];
    fd_set rfds;
    int maxfd = (a > b ? a : b) + 1;
    int a_eof = 0, b_eof = 0;
    while (!a_eof || !b_eof) {
        FD_ZERO(&rfds);
        if (!a_eof) FD_SET(a, &rfds);
        if (!b_eof) FD_SET(b, &rfds);
        if (select(maxfd, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!a_eof && FD_ISSET(a, &rfds)) {
            ssize_t n = read(a, buf, sizeof(buf));
            if (n <= 0) a_eof = 1;
            else if (write_all(b, buf, (size_t)n) < 0) break;
        }
        if (!b_eof && FD_ISSET(b, &rfds)) {
            ssize_t n = read(b, buf, sizeof(buf));
            if (n <= 0) b_eof = 1;
            else if (write_all(a, buf, (size_t)n) < 0) break;
        }
    }
    close(a);
    close(b);
}

static int tcp_connect(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(s); errno = EINVAL; return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s); return -1;
    }
    return s;
}

/* Forks the parent off — this function never returns in the child. */
static void tcp_listener_main(int vport, const char *host, int gport) {
    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { LOG_E("vsock socket() failed: %s", strerror(errno)); _exit(1); }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_vm sa;
    memset(&sa, 0, sizeof(sa));
    sa.svm_family = AF_VSOCK;
    sa.svm_cid    = VMADDR_CID_ANY;
    sa.svm_port   = (unsigned int)vport;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_E("vsock bind(%d) failed: %s", vport, strerror(errno));
        _exit(1);
    }
    if (listen(s, 16) < 0) {
        LOG_E("vsock listen(%d) failed: %s", vport, strerror(errno));
        _exit(1);
    }
    LOG_I("tcp listener: vsock:%d → %s:%d", vport, host, gport);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pid_t child = fork();
        if (child < 0) { close(c); continue; }
        if (child == 0) {
            close(s);
            int t = tcp_connect(host, gport);
            if (t < 0) { LOG_W("tcp connect %s:%d failed: %s", host, gport, strerror(errno)); _exit(1); }
            splice_loop(c, t);
            _exit(0);
        }
        close(c);
    }
    _exit(0);
}

/* ── Config parsing & live edits ────────────────────────────────────────── */

static int parse_uint(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v < 0 || v > 65535) return -1;
    *out = (int)v;
    return 0;
}

/* Spawn a TCP listener child and remember its PID. Idempotent on vport. */
static int spawn_tcp_listener(int vport, const char *host, int gport) {
    if (listener_find(vport) >= 0) return 0;  // already running
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) tcp_listener_main(vport, host, gport);  // never returns
    if (listener_add(vport, pid) < 0) {
        kill(pid, SIGTERM);
        return -1;
    }
    return 0;
}

/* Append a TCP rule to the config file. Best-effort — failure is logged but
 * the in-memory listener is already running, so the rule stays live for this
 * boot. (Config restore on next boot is a soft feature.) */
static void append_config_tcp(int vport, const char *host, int gport) {
    FILE *f = fopen(CONFIG_PATH, "a");
    if (!f) { LOG_W("append %s failed: %s", CONFIG_PATH, strerror(errno)); return; }
    fprintf(f, "%d tcp %s %d\n", vport, host, gport);
    fclose(f);
}

/* Drop the line for vport from the config file. Reads-and-rewrites. */
static void remove_config_line(int vport) {
    FILE *in = fopen(CONFIG_PATH, "r");
    if (!in) return;
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", CONFIG_PATH);
    FILE *out = fopen(tmppath, "w");
    if (!out) { fclose(in); return; }
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        int vp = -1;
        if (sscanf(line, "%d", &vp) == 1 && vp == vport) continue;
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    rename(tmppath, CONFIG_PATH);
}

/* ── Control command handlers ───────────────────────────────────────────── */

static void handle_resize(int rows, int cols) {
    int fd = open("/dev/ttyS0", O_RDONLY | O_NOCTTY);
    if (fd >= 0) {
        struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
        if (ioctl(fd, TIOCSWINSZ, &ws) < 0) {
            LOG_W("TIOCSWINSZ /dev/ttyS0 failed: %s", strerror(errno));
        }
        close(fd);
    }
    FILE *f = fopen(TERM_SIZE_PATH, "w");
    if (f) { fprintf(f, "%d %d\n", rows, cols); fclose(f); }
}

static void handle_add(int vport, const char *host, int gport) {
    if (spawn_tcp_listener(vport, host, gport) == 0) {
        append_config_tcp(vport, host, gport);
        LOG_I("ADD vsock:%d → %s:%d", vport, host, gport);
    } else {
        LOG_W("ADD vsock:%d failed", vport);
    }
}

static void handle_remove(int vport) {
    int idx = listener_find(vport);
    if (idx < 0) { LOG_W("REMOVE vsock:%d — no such listener", vport); return; }
    pid_t pid = listeners[idx].pid;
    kill(pid, SIGTERM);
    listener_remove(idx);
    remove_config_line(vport);
    LOG_I("REMOVE vsock:%d (pid %d)", vport, (int)pid);
}

/* ── Control loop ───────────────────────────────────────────────────────── */

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

static void ctl_loop(int fd) {
    FILE *in = fdopen(dup(fd), "r");
    FILE *out = fdopen(fd, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out); else close(fd);
        return;
    }
    setvbuf(out, NULL, _IOLBF, 0);  // line-buffered replies
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        trim_newline(line);
        if (strncmp(line, "RESIZE ", 7) == 0) {
            int rows = 0, cols = 0;
            if (sscanf(line + 7, "%d %d", &rows, &cols) == 2 && rows > 0 && cols > 0)
                handle_resize(rows, cols);
            else LOG_W("bad RESIZE: '%s'", line);
        } else if (strncmp(line, "ADD ", 4) == 0) {
            int vport = 0, gport = 0;
            char proto[8], host[64];
            if (sscanf(line + 4, "%d %7s %63s %d", &vport, proto, host, &gport) == 4 &&
                strcmp(proto, "tcp") == 0)
                handle_add(vport, host, gport);
            else LOG_W("bad ADD: '%s'", line);
        } else if (strncmp(line, "REMOVE ", 7) == 0) {
            int vport = 0;
            if (sscanf(line + 7, "%d", &vport) == 1)
                handle_remove(vport);
            else LOG_W("bad REMOVE: '%s'", line);
        } else if (strcmp(line, "PING") == 0) {
            fprintf(out, "PONG\n");
            fflush(out);
        } else if (line[0] != '\0') {
            LOG_W("unknown command: '%s'", line);
        }
    }
    fclose(in);
    fclose(out);
}

/* ── Main + config bootstrap ────────────────────────────────────────────── */

static int ctl_vport = -1;

/* Read config; spawn TCP listener children for tcp lines; remember the ctl
 * vport so main() can bind it. */
static void parse_config_and_bootstrap(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { LOG_W("no config at %s — control-only mode", path); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        trim_newline(p);
        int vport = -1;
        char kind[8] = {0};
        if (sscanf(p, "%d %7s", &vport, kind) < 2) continue;
        if (strcmp(kind, "ctl") == 0) {
            ctl_vport = vport;
        } else if (strcmp(kind, "tcp") == 0) {
            char host[64] = {0};
            int gport = 0;
            if (sscanf(p, "%d %*s %63s %d", &vport, host, &gport) == 3) {
                spawn_tcp_listener(vport, host, gport);
            }
        }
    }
    fclose(f);
}

static void reap_children(int signum) {
    (void)signum;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Reap dead listener children; don't accumulate zombies. */
    struct sigaction sa = {0};
    sa.sa_handler = reap_children;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    /* Ignore SIGPIPE — splice_loop's write() will surface the error instead. */
    signal(SIGPIPE, SIG_IGN);

    parse_config_and_bootstrap(CONFIG_PATH);

    if (ctl_vport < 0) {
        LOG_E("no 'ctl' line in %s — exiting", CONFIG_PATH);
        return 1;
    }

    int s = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (s < 0) { LOG_E("ctl socket() failed: %s", strerror(errno)); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_vm sa_v;
    memset(&sa_v, 0, sizeof(sa_v));
    sa_v.svm_family = AF_VSOCK;
    sa_v.svm_cid    = VMADDR_CID_ANY;
    sa_v.svm_port   = (unsigned int)ctl_vport;
    if (bind(s, (struct sockaddr *)&sa_v, sizeof(sa_v)) < 0) {
        LOG_E("ctl bind(%d) failed: %s", ctl_vport, strerror(errno));
        return 1;
    }
    if (listen(s, 4) < 0) {
        LOG_E("ctl listen failed: %s", strerror(errno));
        return 1;
    }
    LOG_I("ctl: listening on vsock:%d", ctl_vport);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        ctl_loop(c);  // runs to EOF; we accept the next connection after
        close(c);
    }
    return 0;
}
