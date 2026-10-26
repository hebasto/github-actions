/*
 * Reproducer for a blocking connect(2) on loopback TCP that never returns
 * on OpenBSD although the kernel reports the connection ESTABLISHED on both
 * ends. Seen in Bitcoin Core's functional tests on OpenBSD 7.9 and 8.0:
 * https://github.com/bitcoin/bitcoin/issues/36099
 *
 * The program mirrors the shape of the failing test:
 *
 *   client process (bitcoind)   proxy process (Python test framework)
 *   ------------------------   ---------------------------------------
 *   N threads per round:        main thread: accept loop on the proxy port,
 *     connect(proxy)              one detached handler thread per connection
 *     send request              handler thread:
 *     recv reply                  recv request, send reply
 *     send 124 bytes              create listener on 127.0.0.1:0 (IP_PORTRANGE_HIGH)
 *     poll for response           hand it to the kqueue acceptor thread, wait for ack
 *                                 connect(listener)            <-- never returns
 *                                 relay bytes both ways until EOF
 *                               acceptor thread: kqueue loop, accepts on every
 *                                 listener, replies 111 bytes to the first data
 *                               watchdog thread: reports a connect() that is
 *                                 pending for longer than -t seconds and exits 2
 *
 * Build:  cc -O2 -Wall -o repro repro.c -lpthread
 * Run:    ./repro [-n connections_per_round] [-r rounds] [-t stuck_seconds] [-b backlog] [-L]
 *         -b sets the proxy listener's backlog (default 128; the Python proxy uses 5).
 *         -L disables IP_PORTRANGE_HIGH on the listeners.
 * Exit:   0 if every round completed, 2 if a hang was detected.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef IP_PORTRANGE
#define IP_PORTRANGE 19
#endif
#ifndef IP_PORTRANGE_HIGH
#define IP_PORTRANGE_HIGH 1
#endif

static int opt_conns = 20;
static int opt_rounds = 200;
static int opt_stuck = 10;
static int opt_portrange_high = 1;
static int opt_backlog = 128;
static uint16_t proxy_port;
static volatile int hang_detected;
static double last_progress;                 /* last completed connection, client side */
static pthread_mutex_t progress_mtx = PTHREAD_MUTEX_INITIALIZER;
static pid_t other_pid;                      /* the peer process, for diagnostics */

static const char *g_role = "?";

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s %d] %s\n", g_role, (int)getpid(), buf);
}

static void die(const char *what)
{
    logmsg("FATAL: %s: %s", what, strerror(errno));
    _exit(3);
}

static void die_eof(const char *what, ssize_t r)
{
    if (r == 0) {
        logmsg("FATAL: %s: unexpected end of stream", what);
        _exit(3);
    }
    die(what);
}

static void set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) die("fcntl(F_GETFL)");
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, fl) < 0) die("fcntl(F_SETFL)");
}

static int lo_addr(struct sockaddr_in *sin, uint16_t port)
{
    memset(sin, 0, sizeof *sin);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin->sin_port = htons(port);
    return sizeof *sin;
}

static uint16_t local_port(int fd)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    if (getsockname(fd, (struct sockaddr *)&sin, &len) < 0) die("getsockname");
    return ntohs(sin.sin_port);
}

static void run_cmd(const char *cmd)
{
    char line[512];
    FILE *p = popen(cmd, "r");
    if (!p) return;
    fprintf(stderr, "--- %s\n", cmd);
    while (fgets(line, sizeof line, p)) fputs(line, stderr);
    pclose(p);
}

/* ------------------------------------------------------------------------
 * Slot table: every pending blocking call records itself here so that the
 * watchdog thread can report the ones that do not return.
 * ---------------------------------------------------------------------- */

enum { ST_FREE = 0, ST_PENDING = 1 };

struct slot {
    int state;
    double since;
    int round;
    int index;
    int fd;
    uint16_t port;
    const char *what;
    pthread_t thread;
};

#define NSLOTS 4096
static struct slot slots[NSLOTS];
static pthread_mutex_t slots_mtx = PTHREAD_MUTEX_INITIALIZER;

static int slot_begin(const char *what, int round, int index, int fd, uint16_t port)
{
    int i;
    pthread_mutex_lock(&slots_mtx);
    for (i = 0; i < NSLOTS; i++) {
        if (slots[i].state == ST_FREE) {
            slots[i].state = ST_PENDING;
            slots[i].since = now_s();
            slots[i].round = round;
            slots[i].index = index;
            slots[i].fd = fd;
            slots[i].port = port;
            slots[i].what = what;
            slots[i].thread = pthread_self();
            break;
        }
    }
    pthread_mutex_unlock(&slots_mtx);
    if (i == NSLOTS) die("slot table full");
    return i;
}

static void slot_end(int i)
{
    pthread_mutex_lock(&slots_mtx);
    slots[i].state = ST_FREE;
    pthread_mutex_unlock(&slots_mtx);
}

static void dump_state(uint16_t port)
{
    char cmd[256];
    if (port) {
        snprintf(cmd, sizeof cmd, "netstat -an -p tcp | grep -E 'Recv-Q|\\.%u '", port);
        run_cmd(cmd);
    }
    run_cmd("netstat -an -p tcp | awk '$2 > 0 || $3 > 0'");
    run_cmd("netstat -an -p tcp | grep -c ESTABLISHED");
    snprintf(cmd, sizeof cmd, "fstat -p %d | grep -E 'USER|internet'", (int)getpid());
    run_cmd(cmd);
    snprintf(cmd, sizeof cmd, "ps -H -o pid,tid,wchan,state,command -p %d", (int)getpid());
    run_cmd(cmd);
    if (other_pid > 0) {
        snprintf(cmd, sizeof cmd, "fstat -p %d | grep -E 'USER|internet'", (int)other_pid);
        run_cmd(cmd);
        snprintf(cmd, sizeof cmd, "ps -H -o pid,tid,wchan,state,command -p %d", (int)other_pid);
        run_cmd(cmd);
    }
    fflush(stderr);
}

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        double t = now_s();
        int i;
        usleep(500 * 1000);
        pthread_mutex_lock(&slots_mtx);
        for (i = 0; i < NSLOTS; i++) {
            struct slot *s = &slots[i];
            if (s->state == ST_PENDING && t - s->since > opt_stuck && !hang_detected) {
                logmsg("HANG: %s pending for %.1f s (round %d, connection %d, fd %d, port %u)",
                       s->what, t - s->since, s->round, s->index, s->fd, s->port);
                pthread_mutex_unlock(&slots_mtx);
                dump_state(s->port);
                hang_detected = 1;
                /* A stuck client thread can never be joined: end the client here.
                 * The proxy keeps running so the client can still report it. */
                if (other_pid == getppid()) _exit(2);
                pthread_mutex_lock(&slots_mtx);
                break;
            }
        }
        pthread_mutex_unlock(&slots_mtx);
        /* Client side: no connection completed for a long time, whatever is stuck. */
        pthread_mutex_lock(&progress_mtx);
        if (last_progress > 0 && t - last_progress > 3 * opt_stuck) {
            logmsg("HANG: no connection completed for %.1f s; dumping both processes", t - last_progress);
            pthread_mutex_unlock(&progress_mtx);
            dump_state(0);
            hang_detected = 1;
            _exit(2);
        }
        pthread_mutex_unlock(&progress_mtx);
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Acceptor thread: a kqueue event loop that owns every listener the handler
 * threads create, like the asyncio network thread of the test framework.
 * ---------------------------------------------------------------------- */

#define MAXFD 65536
static struct {
    unsigned char listening;
    unsigned char replied;
} fdstate[MAXFD];

static int kq_wakeup[2];               /* socketpair, [0] in acceptor, [1] in handlers */
static int reg_queue[1024];
static int reg_head, reg_tail;
static int reg_acked[MAXFD];
static pthread_mutex_t reg_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reg_cv = PTHREAD_COND_INITIALIZER;

static const char response[111] = "response";

static void *acceptor(void *arg)
{
    int kq = *(int *)arg;
    struct kevent evs[64];
    for (;;) {
        int n = kevent(kq, NULL, 0, evs, 64, NULL);
        int i;
        if (n < 0) {
            if (errno == EINTR) continue;
            die("kevent(wait)");
        }
        for (i = 0; i < n; i++) {
            int fd = (int)evs[i].ident;
            if (fd == kq_wakeup[0]) {
                char b[64];
                while (read(fd, b, sizeof b) > 0) {}
                pthread_mutex_lock(&reg_mtx);
                while (reg_head != reg_tail) {
                    int lfd = reg_queue[reg_head];
                    struct kevent ch;
                    reg_head = (reg_head + 1) % 1024;
                    EV_SET(&ch, lfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    if (kevent(kq, &ch, 1, NULL, 0, NULL) < 0) die("kevent(add listener)");
                    fdstate[lfd].listening = 1;
                    reg_acked[lfd] = 1;
                }
                pthread_cond_broadcast(&reg_cv);
                pthread_mutex_unlock(&reg_mtx);
            } else if (fdstate[fd].listening) {
                for (;;) {
                    struct kevent ch;
                    int c = accept(fd, NULL, NULL);
                    if (c < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR || errno == EBADF) break;
                        die("accept(listener)");
                    }
                    set_nonblock(c, 1);
                    fdstate[c].listening = 0;
                    fdstate[c].replied = 0;
                    EV_SET(&ch, c, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    if (kevent(kq, &ch, 1, NULL, 0, NULL) < 0) die("kevent(add conn)");
                }
            } else {
                char buf[4096];
                ssize_t r = read(fd, buf, sizeof buf);
                if (r > 0) {
                    if (!fdstate[fd].replied) {
                        fdstate[fd].replied = 1;
                        if (write(fd, response, sizeof response) < 0 && errno != EPIPE) die("write(response)");
                    }
                } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                    close(fd);
                }
            }
        }
    }
    return NULL;
}

/* Create a listener on 127.0.0.1:0, register it with the acceptor, wait for
 * the registration to be acknowledged, and return its fd. */
static int make_listener(uint16_t *port)
{
    struct sockaddr_in sin;
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket(listener)");
    if (opt_portrange_high) {
        int high = IP_PORTRANGE_HIGH;
        if (setsockopt(fd, IPPROTO_IP, IP_PORTRANGE, &high, sizeof high) < 0) die("setsockopt(IP_PORTRANGE)");
    }
    (void)one;
    lo_addr(&sin, 0);
    if (bind(fd, (struct sockaddr *)&sin, sizeof sin) < 0) die("bind(listener)");
    if (listen(fd, 128) < 0) die("listen(listener)");
    set_nonblock(fd, 1);
    *port = local_port(fd);

    pthread_mutex_lock(&reg_mtx);
    reg_acked[fd] = 0;
    reg_queue[reg_tail] = fd;
    reg_tail = (reg_tail + 1) % 1024;
    if (write(kq_wakeup[1], "x", 1) < 0) die("write(wakeup)");
    while (!reg_acked[fd]) pthread_cond_wait(&reg_cv, &reg_mtx);
    pthread_mutex_unlock(&reg_mtx);
    return fd;
}

/* ------------------------------------------------------------------------
 * Handler thread: one per accepted proxy connection, like Socks5Connection.
 * ---------------------------------------------------------------------- */

struct handler_arg {
    int conn;
    int seq;
};

static ssize_t recv_exact(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r <= 0) return r;
        got += r;
    }
    return got;
}

static void relay(int a, int b)
{
    struct pollfd pfd[2];
    char buf[4096];
    set_nonblock(a, 1);
    set_nonblock(b, 1);
    for (;;) {
        int i;
        pfd[0].fd = a; pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = b; pfd[1].events = POLLIN; pfd[1].revents = 0;
        if (poll(pfd, 2, 2000) < 0) {
            if (errno == EINTR) continue;
            die("poll(relay)");
        }
        for (i = 0; i < 2; i++) {
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                int from = pfd[i].fd, to = (i == 0) ? b : a;
                ssize_t r = read(from, buf, sizeof buf);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) return;
                if (r > 0) {
                    ssize_t off = 0;
                    while (off < r) {
                        struct pollfd w = { to, POLLOUT, 0 };
                        ssize_t w_n;
                        poll(&w, 1, -1);
                        w_n = write(to, buf + off, r - off);
                        if (w_n < 0) {
                            if (errno == EAGAIN || errno == EINTR) continue;
                            return;
                        }
                        off += w_n;
                    }
                }
            }
        }
    }
}

static void *handler(void *arg)
{
    struct handler_arg *h = arg;
    int conn = h->conn, seq = h->seq;
    char req[3];
    uint16_t port;
    int lfd, s, slot;
    struct sockaddr_in sin;
    free(h);

    slot = slot_begin("proxy recv() of request", seq / opt_conns, seq % opt_conns, conn, proxy_port);
    {
        ssize_t r = recv_exact(conn, req, sizeof req);
        if (r != (ssize_t)sizeof req) {
            logmsg("handler %d: request not received (%s)", seq, r == 0 ? "end of stream" : strerror(errno));
            slot_end(slot);
            goto out;
        }
    }
    slot_end(slot);
    if (send(conn, "ok", 2, 0) != 2) goto out;

    slot = slot_begin("proxy listener registration", seq / opt_conns, seq % opt_conns, -1, 0);
    lfd = make_listener(&port);
    slot_end(slot);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket(relay)");
    lo_addr(&sin, port);
    slot = slot_begin("proxy connect() to listener", seq / opt_conns, seq % opt_conns, s, port);
    if (connect(s, (struct sockaddr *)&sin, sizeof sin) < 0) die("connect(listener)");
    slot_end(slot);

    relay(conn, s);
    close(s);
    close(lfd);
out:
    close(conn);
    return NULL;
}

static int proxy_listen_fd;

static void *proxy_main(void *arg)
{
    int proxy_fd = proxy_listen_fd;
    pthread_t t;
    int kq = kqueue();
    (void)arg;
    int seq = 0;
    if (kq < 0) die("kqueue");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, kq_wakeup) < 0) die("socketpair");
    set_nonblock(kq_wakeup[0], 1);
    {
        struct kevent ch;
        EV_SET(&ch, kq_wakeup[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(kq, &ch, 1, NULL, 0, NULL) < 0) die("kevent(add wakeup)");
    }
    if (pthread_create(&t, NULL, acceptor, &kq)) die("pthread_create(acceptor)");
    if (pthread_create(&t, NULL, watchdog, NULL)) die("pthread_create(watchdog)");

    for (;;) {
        pthread_attr_t attr;
        struct handler_arg *h;
        int c = accept(proxy_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            die("accept(proxy)");
        }
        h = malloc(sizeof *h);
        h->conn = c;
        h->seq = seq++;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, handler, h)) die("pthread_create(handler)");
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Client process: N threads per round, each like one bitcoind connection.
 * ---------------------------------------------------------------------- */

static pthread_barrier_t round_barrier;
static int client_failed;
static double round_max_ms;
static pthread_mutex_t round_mtx = PTHREAD_MUTEX_INITIALIZER;

struct client_arg { int round; int index; };

static void *client_thread(void *arg)
{
    struct client_arg *c = arg;
    struct sockaddr_in sin;
    char reply[2], version[124], buf[256];
    struct pollfd pfd;
    int s, slot, r;
    double t0, dt;

    memset(version, 'v', sizeof version);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket(client)");
    lo_addr(&sin, proxy_port);

    pthread_barrier_wait(&round_barrier);
    t0 = now_s();
    slot = slot_begin("client connect() to proxy", c->round, c->index, s, proxy_port);
    if (connect(s, (struct sockaddr *)&sin, sizeof sin) < 0) die("connect(proxy)");
    slot_end(slot);
    slot = slot_begin("client send() of request", c->round, c->index, s, proxy_port);
    if (send(s, "req", 3, 0) != 3) die("send(request)");
    slot_end(slot);
    slot = slot_begin("client recv() of proxy reply", c->round, c->index, s, proxy_port);
    r = recv_exact(s, reply, 2);
    if (r != 2) die_eof("recv(reply)", r);
    slot_end(slot);
    slot = slot_begin("client send() of version", c->round, c->index, s, proxy_port);
    if (send(s, version, sizeof version, 0) != (ssize_t)sizeof version) die("send(version)");
    slot_end(slot);

    pfd.fd = s; pfd.events = POLLIN; pfd.revents = 0;
    r = poll(&pfd, 1, 2 * opt_stuck * 1000);
    if (r == 0) {
        logmsg("HANG: client got no response through the proxy after %d s (round %d, connection %d, local port %u)",
               2 * opt_stuck, c->round, c->index, local_port(s));
        pthread_mutex_lock(&round_mtx);
        client_failed = 1;
        pthread_mutex_unlock(&round_mtx);
    } else if (r < 0) {
        die("poll(client)");
    } else if (recv(s, buf, sizeof buf, 0) <= 0) {
        logmsg("client: connection closed without a response (round %d, connection %d)", c->round, c->index);
        pthread_mutex_lock(&round_mtx);
        client_failed = 1;
        pthread_mutex_unlock(&round_mtx);
    }
    dt = (now_s() - t0) * 1000;
    pthread_mutex_lock(&round_mtx);
    if (dt > round_max_ms) round_max_ms = dt;
    pthread_mutex_unlock(&round_mtx);
    pthread_mutex_lock(&progress_mtx);
    last_progress = now_s();
    pthread_mutex_unlock(&progress_mtx);
    close(s);
    free(c);
    return NULL;
}

static int client_main(void)
{
    pthread_t wd;
    int round;
    other_pid = getppid();
    pthread_mutex_lock(&progress_mtx);
    last_progress = now_s();
    pthread_mutex_unlock(&progress_mtx);
    if (pthread_create(&wd, NULL, watchdog, NULL)) die("pthread_create(watchdog)");
    for (round = 0; round < opt_rounds; round++) {
        pthread_t th[1024];
        int i;
        if (opt_conns > 1024) die("too many connections");
        pthread_barrier_init(&round_barrier, NULL, opt_conns);
        round_max_ms = 0;
        for (i = 0; i < opt_conns; i++) {
            struct client_arg *c = malloc(sizeof *c);
            c->round = round;
            c->index = i;
            if (pthread_create(&th[i], NULL, client_thread, c)) die("pthread_create(client)");
        }
        for (i = 0; i < opt_conns; i++) pthread_join(th[i], NULL);
        pthread_barrier_destroy(&round_barrier);
        if (client_failed || hang_detected) {
            logmsg("round %d: FAILED", round);
            return 2;
        }
        if (round % 10 == 0 || round == opt_rounds - 1)
            logmsg("round %d: ok, slowest connection %.0f ms", round, round_max_ms);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ch, proxy_fd, one = 1;
    struct sockaddr_in sin;
    pid_t pid;
    int status;

    while ((ch = getopt(argc, argv, "n:r:t:b:L")) != -1) {
        switch (ch) {
        case 'n': opt_conns = atoi(optarg); break;
        case 'r': opt_rounds = atoi(optarg); break;
        case 't': opt_stuck = atoi(optarg); break;
        case 'b': opt_backlog = atoi(optarg); break;
        case 'L': opt_portrange_high = 0; break;
        default:
            fprintf(stderr, "usage: %s [-n connections] [-r rounds] [-t stuck_seconds] [-b backlog] [-L]\n", argv[0]);
            return 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    /* The proxy listener, created before the fork so the client knows the port. */
    proxy_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) die("socket(proxy)");
    setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (opt_portrange_high) {
        int high = IP_PORTRANGE_HIGH;
        setsockopt(proxy_fd, IPPROTO_IP, IP_PORTRANGE, &high, sizeof high);
    }
    lo_addr(&sin, 0);
    if (bind(proxy_fd, (struct sockaddr *)&sin, sizeof sin) < 0) die("bind(proxy)");
    if (listen(proxy_fd, opt_backlog) < 0) die("listen(proxy)");
    proxy_port = local_port(proxy_fd);
    fprintf(stderr, "proxy on 127.0.0.1:%u (backlog %d), %d connections per round, %d rounds, stuck after %d s, IP_PORTRANGE_HIGH %s\n",
            proxy_port, opt_backlog, opt_conns, opt_rounds, opt_stuck, opt_portrange_high ? "on" : "off");

    pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        g_role = "client";
        close(proxy_fd);
        _exit(client_main());
    }
    g_role = "proxy";
    other_pid = pid;
    {
        pthread_t t;
        /* Run the proxy in a thread so the main thread can wait for the client. */
        proxy_listen_fd = proxy_fd;
        if (pthread_create(&t, NULL, proxy_main, NULL)) die("pthread_create(proxy)");
    }
    if (waitpid(pid, &status, 0) < 0) die("waitpid");
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0 && hang_detected) code = 2;
        logmsg("client exited with status %d: %s", WEXITSTATUS(status), code == 0 ? "no hang detected" : "HANG DETECTED");
        return code;
    }
    logmsg("client terminated abnormally");
    return 3;
}
