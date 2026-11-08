/* The part of AtariOS the store's Unity games talk to: its bundle bridge
 * daemon. Games built with Atari's Unity SDK (AtariUnitySDK.dll) connect to
 * $XDG_RUNTIME_DIR/bundle-bridge-daemon at start, a Unix socket carrying
 * JSON-RPC 2.0 one message a line (StreamJsonRpc's NewLineDelimitedMessage-
 * Handler), and ask for the user (user/getUser -> { token }) and the system
 * (system/getInformation -> { os_variant }). Only with both answered do the
 * Atari services count as initialised; without the daemon Breakout
 * Recharged stays on "Press [] To play" for good. This answers as a VCS
 * with one user would.
 *
 * The game gets a runtime folder of its own holding the socket, with links
 * to everything in the real one (PulseAudio's socket and the rest). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "daemon.h"

#define MAX_CLIENTS 8
#define LINE_MAX_LEN 65536

/* The user every game sees: the SDK's own stand-in when run in Unity's
 * editor */
#define USER_TOKEN "77db3449-bbd6-455e-972d-5b99f1479c35"

static char run_dir[256];
static char sock_path[300];
static int listen_fd = -1;
static int wake[2] = { -1, -1 };
static pthread_t thread;
static bool thread_running;
static char store_id[128], bundle_name[256];
static void (*log_fn)(const char *);

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_fn)
        log_fn(buf);
}

/* A JSON string's characters as JSON wants them */
static void json_escape(char *out, size_t size, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, size - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* The raw text of a member's value: a string with its quotes, or a number;
 * enough for "id" and "method" in the requests the SDK sends */
static bool member(const char *json, const char *name, char *out, size_t size)
{
    char key[64];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *p = strstr(json, key);
    if (!p)
        return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
    const char *end = p;
    if (*p == '"') {
        end++;
        while (*end && *end != '"') {
            if (*end == '\\' && end[1])
                end++;
            end++;
        }
        if (*end == '"')
            end++;
    } else {
        while (*end && *end != ',' && *end != '}' && *end != ' ')
            end++;
    }
    size_t n = (size_t)(end - p);
    if (n + 1 > size)
        return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static void send_all(int fd, const char *s)
{
    size_t left = strlen(s);
    while (left) {
        ssize_t w = send(fd, s, left, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return;
        }
        s += w;
        left -= (size_t)w;
    }
}

static void answer(int fd, const char *line)
{
    char id[128], method[128], reply[1024];
    if (!member(line, "method", method, sizeof(method)))
        return;
    if (!member(line, "id", id, sizeof(id))) {
        say("atarivcs daemon: notification %s\n", method);
        return; /* a notification: nothing to answer */
    }
    if (!strcmp(method, "\"user/getUser\"")) {
        snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"token\":\"%s\"}}\n", id, USER_TOKEN);
    } else if (!strcmp(method, "\"system/getInformation\"")) {
        snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"os_variant\":\"production\"}}\n", id);
    } else if (!strcmp(method, "\"bundles/getRunningBundle\"")) {
        char sid[256], name[512];
        json_escape(sid, sizeof(sid), store_id);
        json_escape(name, sizeof(name), bundle_name);
        snprintf(reply, sizeof(reply), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"store_id\":\"%s\",\"name\":\"%s\"}}\n",
                 id, sid, name);
    } else {
        snprintf(reply, sizeof(reply),
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n", id);
    }
    say("atarivcs daemon: %s answered\n", method);
    send_all(fd, reply);
}

static void *serve(void *arg)
{
    (void)arg;
    struct { int fd; char *buf; size_t len; } clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    for (;;) {
        struct pollfd pfd[MAX_CLIENTS + 2];
        int n = 0;
        pfd[n++] = (struct pollfd){ .fd = wake[0], .events = POLLIN };
        pfd[n++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        for (int i = 0; i < MAX_CLIENTS; i++)
            pfd[n++] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };
        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfd[0].revents)
            break; /* stop */
        if (pfd[1].revents & POLLIN) {
            int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                if (slot < 0) {
                    close(c);
                } else {
                    clients[slot].fd = c;
                    clients[slot].len = 0;
                    if (!clients[slot].buf)
                        clients[slot].buf = malloc(LINE_MAX_LEN);
                    say("atarivcs daemon: a game connected\n");
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0 || !(pfd[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char *buf = clients[i].buf;
            ssize_t got = buf ? recv(clients[i].fd, buf + clients[i].len, LINE_MAX_LEN - 1 - clients[i].len, 0) : -1;
            if (got <= 0) {
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }
            clients[i].len += (size_t)got;
            buf[clients[i].len] = '\0';
            char *start = buf, *nl;
            while ((nl = strchr(start, '\n'))) {
                *nl = '\0';
                if (nl > start && nl[-1] == '\r')
                    nl[-1] = '\0';
                if (*start)
                    answer(clients[i].fd, start);
                start = nl + 1;
            }
            clients[i].len = strlen(start);
            memmove(buf, start, clients[i].len + 1);
            if (clients[i].len >= LINE_MAX_LEN - 1)
                clients[i].len = 0; /* a line too long for anything the SDK sends */
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0)
            close(clients[i].fd);
        free(clients[i].buf);
    }
    return NULL;
}

/* Links in run_dir to every entry of the real runtime folder */
static void link_runtime(const char *real)
{
    DIR *d = opendir(real);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "bundle-bridge-daemon"))
            continue;
        char from[512], to[512];
        snprintf(from, sizeof(from), "%s/%s", real, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", run_dir, e->d_name);
        if (symlink(from, to) != 0 && errno != EEXIST)
            say("atarivcs daemon: no link to %s\n", from);
    }
    closedir(d);
}

static void remove_run_dir(void)
{
    if (!run_dir[0])
        return;
    DIR *d = opendir(run_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", run_dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(run_dir);
    run_dir[0] = '\0';
}

bool atarivcs_daemon_start(const char *store, const char *name, void (*log)(const char *))
{
    log_fn = log;
    snprintf(store_id, sizeof(store_id), "%s", store ? store : "");
    snprintf(bundle_name, sizeof(bundle_name), "%s", name ? name : "");
    snprintf(run_dir, sizeof(run_dir), "/tmp/atarivcs-%d-run", (int)getpid());
    remove_run_dir();
    snprintf(run_dir, sizeof(run_dir), "/tmp/atarivcs-%d-run", (int)getpid());
    if (mkdir(run_dir, 0700) != 0) {
        say("atarivcs daemon: mkdir %s: %s\n", run_dir, strerror(errno));
        run_dir[0] = '\0';
        return false;
    }
    const char *real = getenv("XDG_RUNTIME_DIR");
    if (real && *real)
        link_runtime(real);

    snprintf(sock_path, sizeof(sock_path), "%s/bundle-bridge-daemon", run_dir);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 4) != 0 ||
        pipe2(wake, O_CLOEXEC) != 0) {
        say("atarivcs daemon: socket %s: %s\n", sock_path, strerror(errno));
        atarivcs_daemon_stop();
        return false;
    }
    if (pthread_create(&thread, NULL, serve, NULL) != 0) {
        atarivcs_daemon_stop();
        return false;
    }
    thread_running = true;
    return true;
}

const char *atarivcs_daemon_runtime_dir(void)
{
    return run_dir[0] ? run_dir : NULL;
}

void atarivcs_daemon_stop(void)
{
    if (thread_running) {
        if (write(wake[1], "x", 1) < 0) {
            /* the thread then ends when its socket closes */
        }
        pthread_join(thread, NULL);
        thread_running = false;
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    for (int i = 0; i < 2; i++)
        if (wake[i] >= 0) {
            close(wake[i]);
            wake[i] = -1;
        }
    if (sock_path[0]) {
        unlink(sock_path);
        sock_path[0] = '\0';
    }
    remove_run_dir();
}
