/*
 * extra-type-hotkey — compositor-independent global hotkey for extra-type.
 *
 * Grabs every physical keyboard at the evdev layer (EVIOCGRAB) and passes
 * every key through to a /dev/uinput virtual keyboard untouched, so typing
 * and existing bindings are unaffected. When the configured combo is pressed,
 * the combo key is swallowed (not forwarded) and a POST is made to the daemon
 * /toggle endpoint.
 *
 * Pure libc + Linux kernel headers. No external runtime dependencies.
 *
 * Usage:
 *   extra-type-hotkey --port 3232 --combo ctrl+alt+space [--dry] [--log-file PATH]
 *
 * The combo is "mods+evdevcode" where mods are any of ctrl/alt/super/shift and
 * evdevcode is the numeric evdev key code, e.g. ctrl+shift+47 (space).
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS 32
#define NUM_KEY (KEY_MAX + 1)

/* modifier bitmask */
#define M_CTRL 1
#define M_ALT 2
#define M_SUPER 4
#define M_SHIFT 8

typedef struct {
    int fd;
    char name[256];
} HotkeyDev;

static HotkeyDev devs[MAX_DEVS];
static int dev_count = 0;
static int uinput_fd = -1;
static int dry = 0;
static int port = 3232;
static int target_code = -1;
static unsigned required_mods = 0;
static uint8_t key_down[NUM_KEY];
static int swallow_target = 0;
static FILE *logf = NULL;
static volatile sig_atomic_t stop_requested = 0;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void putlog(const char *fmt, ...) {
    va_list ap;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(stderr, "[extra-type-hotkey %s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (logf) {
        fprintf(logf, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(logf, fmt, ap);
        va_end(ap);
        fputc('\n', logf);
        fflush(logf);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP                                                               */
/* ------------------------------------------------------------------ */
static int http_request(const char *path, int *status_out) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    struct pollfd pfd = {sock, POLLOUT, 0};
    if (poll(&pfd, 1, 1000) <= 0 || !(pfd.revents & POLLOUT)) {
        close(sock);
        return -1;
    }

    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(sock);
        return -1;
    }

    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
                      path, port);
    if (send(sock, req, (size_t)rl, 0) < 0) {
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);
    struct pollfd rp = {sock, POLLIN, 0};
    if (poll(&rp, 1, 2000) <= 0) {
        close(sock);
        return -1;
    }

    char buf[1024];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (n <= 0) return -1;
    buf[n] = '\0';

    if (status_out) *status_out = 0;
    const char *proto = buf;
    if (strncasecmp(proto, "HTTP/1.", 7) == 0) {
        int code = atoi(proto + 9);
        if (status_out) *status_out = code;
    }
    return 0;
}

static void do_toggle(void) {
    int status = 0;
    int rc = http_request("/toggle", &status);
    if (rc < 0) putlog("toggle: daemon unreachable on 127.0.0.1:%d", port);
    else putlog("toggle: sent, HTTP %d", status);
}

static int do_health(void) {
    int status = 0;
    if (http_request("/health", &status) < 0) return -1;
    return (status == 200) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* uinput passthrough                                                 */
/* ------------------------------------------------------------------ */
static void uinject(__u16 type, __u16 code, __s32 value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        /* device may be gone; best effort */
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    (void)write(uinput_fd, &ev, sizeof(ev));
}

static int create_uinput(void) {
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        putlog("cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        putlog("UI_SET_EVBIT failed: %s", strerror(errno));
        return -1;
    }
    for (int code = 0; code < NUM_KEY; code++) {
        (void)ioctl(uinput_fd, UI_SET_KEYBIT, code);
    }

    struct uinput_setup uset;
    memset(&uset, 0, sizeof(uset));
    uset.id.bustype = BUS_USB;
    uset.id.vendor = 0x238a;
    uset.id.product = 0x0184;
    uset.id.version = 1;
    strncpy((char *)uset.name, "extra-type hotkey passthrough",
            sizeof(uset.name) - 1);
    if (ioctl(uinput_fd, UI_DEV_SETUP, &uset) < 0) {
        putlog("UI_DEV_SETUP failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        putlog("UI_DEV_CREATE failed: %s", strerror(errno));
        return -1;
    }
    putlog("uinput device created");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Combo parsing                                                      */
/* ------------------------------------------------------------------ */
static int combo_mod_bit(const char *s) {
    if (strcasecmp(s, "ctrl") == 0 || strcasecmp(s, "control") == 0) return M_CTRL;
    if (strcasecmp(s, "alt") == 0) return M_ALT;
    if (strcasecmp(s, "super") == 0 || strcasecmp(s, "meta") == 0) return M_SUPER;
    if (strcasecmp(s, "shift") == 0) return M_SHIFT;
    return 0;
}

static int parse_combo(const char *spec) {
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok;
    int have_key = 0;
    required_mods = 0;

    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        char *end = NULL;
        long code = strtol(tok, &end, 10);
        if (end && *end == '\0' && code > 0 && code < NUM_KEY && code != target_code) {
            target_code = (int)code;
            have_key = 1;
        } else {
            required_mods |= (unsigned)combo_mod_bit(tok);
        }
    }
    if (target_code < 0 && have_key) {
        target_code = 0;
    }
    if (!have_key || required_mods == 0) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Device enumeration + grab                                          */
/* ------------------------------------------------------------------ */
static int test_bit_q(const unsigned long *bits, int code) {
    return (bits[code >> 6] & (1UL << (code & 63))) ? 1 : 0;
}

static void paint_key_bits(int fd, unsigned long **out) {
    *out = NULL;
    unsigned long *bits = calloc((size_t)NUM_KEY * 8, 1);
    if (!bits) return;
    /* Some kernels return 0 for EVIOCGBIT(ev, 0) with no buffer; always read
     * a full key bitmap (NUM_KEY bits) regardless of the probe result. */
    if (ioctl(fd, EVIOCGBIT(EV_KEY, NUM_KEY), bits) < 0) {
        free(bits);
        return;
    }
    *out = bits;
}

static int is_keyboard(const unsigned long *key, const char *name) {
    if (!key) return 0;
    if (strstr(name, "extra-type") != NULL) return 0;
    if (!test_bit_q(key, KEY_A)) return 0;
    if (!test_bit_q(key, KEY_ENTER) && !test_bit_q(key, KEY_SPACE)) return 0;
    return 1;
}

static int grab_devices(void) {
    DIR *d = opendir("/dev/input");
    if (!d) {
        putlog("cannot open /dev/input: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && dev_count < MAX_DEVS) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDWR);
        if (fd < 0) continue;

        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            name[0] = '\0';
        }
        unsigned long *key = NULL;
        paint_key_bits(fd, &key);

        int is_kbd = is_keyboard(key, name);
        if (key) free(key);

        int abs_sz = (int)ioctl(fd, EVIOCGBIT(EV_ABS, 0), NULL);
        if (is_kbd && abs_sz > 0) {
            is_kbd = 0; /* touchpads etc. expose ABS with keyboard-ish bits */
        }

        if (!is_kbd) {
            close(fd);
            continue;
        }

        if (!dry) {
            if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
                /* e.g. keyd already holds the physical devices; skip them
                 * so we can still grab the keyd virtual keyboard. */
                putlog("EVIOCGRAB failed on %s (%s): %s (skipping)",
                       path, name, strerror(errno));
                close(fd);
                continue;
            }
        }

        devs[dev_count].fd = fd;
        snprintf(devs[dev_count].name, sizeof(devs[dev_count].name), "%s", name);
        putlog("%s %s (%s)", dry ? "watching" : "grabbed", path, name);
        dev_count++;
    }
    closedir(d);

    if (dev_count == 0) {
        putlog("no keyboard devices found; check user permissions (%s)",
               strerror(errno));
        return -1;
    }
    putlog("%d keyboard(s) %s", dev_count, dry ? "watched" : "grabbed");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Event handling                                                     */
/* ------------------------------------------------------------------ */
static int all_mods_down(void) {
    if (required_mods & M_CTRL)
        if (!key_down[KEY_LEFTCTRL] && !key_down[KEY_RIGHTCTRL]) return 0;
    if (required_mods & M_ALT)
        if (!key_down[KEY_LEFTALT] && !key_down[KEY_RIGHTALT]) return 0;
    if (required_mods & M_SUPER)
        if (!key_down[KEY_LEFTMETA] && !key_down[KEY_RIGHTMETA]) return 0;
    if (required_mods & M_SHIFT)
        if (!key_down[KEY_LEFTSHIFT] && !key_down[KEY_RIGHTSHIFT]) return 0;
    return 1;
}

static void handle_event(struct input_event *ev) {
    if (ev->type != EV_KEY) {
        if (!dry) uinject(ev->type, ev->code, ev->value);
        return;
    }

    if (ev->value == 2) { /* auto-repeat: forward verbatim, never toggle */
        if (!dry) uinject(EV_KEY, ev->code, 2);
        return;
    }

    if (ev->code == target_code) {
        if (ev->value == 1) {
            if (all_mods_down()) {
                static long long last = 0;
                long long now = now_ms();
                if (now - last < 400) {
                    return; /* debounce: swallow the double */
                }
                last = now;
                swallow_target = 1;
                putlog("combo pressed, toggling");
                do_toggle();
                return;
            }
            swallow_target = 0;
        } else if (ev->value == 0) {
            if (swallow_target) {
                swallow_target = 0;
                return; /* swallow the release of the trigger key */
            }
        }
        /* fall through to forward + state tracking */
    }

    if ((unsigned)ev->code < NUM_KEY) {
        key_down[ev->code] = (uint8_t)(ev->value ? 1 : 0);
    }
    if (!dry) uinject(EV_KEY, ev->code, ev->value);
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                            */
/* ------------------------------------------------------------------ */
static void release_held_keys(void) {
    if (dry || uinput_fd < 0) return;
    for (int code = 0; code < NUM_KEY; code++) {
        if (key_down[code]) {
            uinject(EV_KEY, (__u16)code, 0);
            key_down[code] = 0;
        }
    }
}

static void cleanup_and_exit(int code) {
    stop_requested = 1;
    release_held_keys();
    for (int i = 0; i < dev_count; i++) {
        if (devs[i].fd >= 0) close(devs[i].fd);
    }
    if (uinput_fd >= 0) {
        close(uinput_fd); /* destroys the virtual device, releasing grab */
        uinput_fd = -1;
    }
    if (logf) {
        fclose(logf);
        logf = NULL;
    }
    exit(code);
}

static void on_signal(int sig) {
    (void)sig;
    putlog("signal received, cleaning up");
    cleanup_and_exit(0);
}

/* ------------------------------------------------------------------ */
static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --port PORT --combo 'ctrl+alt+<evdevcode>' [--dry] "
            "[--log-file PATH]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *combo = NULL;
    const char *logpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--combo") == 0 && i + 1 < argc) {
            combo = argv[++i];
        } else if (strcmp(argv[i], "--dry") == 0) {
            dry = 1;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            logpath = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!combo || parse_combo(combo) < 0) {
        if (combo) {
            putlog("invalid combo '%s': expected 'mods+evdevcode' "
                   "(e.g. ctrl+alt+57 = Ctrl+Alt+Space)", combo);
        }
        usage(argv[0]);
        return 2;
    }

    if (logpath) {
        logf = fopen(logpath, "a");
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (!dry && create_uinput() < 0) {
        return 1;
    }

    if (grab_devices() < 0) {
        if (!dry && uinput_fd >= 0) close(uinput_fd);
        return 1;
    }

    putlog("watching combo ctrl=%s alt=%s super=%s shift=%s key=%d on port %d%s",
           (required_mods & M_CTRL) ? "on" : "off",
           (required_mods & M_ALT) ? "on" : "off",
           (required_mods & M_SUPER) ? "on" : "off",
           (required_mods & M_SHIFT) ? "on" : "off",
           target_code, port, dry ? " (dry-run)" : "");

    long long next_health = now_ms() + 5000;
    int health_fails = 0;

    struct pollfd pfds[MAX_DEVS];
    while (!stop_requested) {
        if (!dry) {
            if (now_ms() >= next_health) {
                next_health = now_ms() + 5000;
                if (do_health() < 0) {
                    health_fails++;
                    if (health_fails >= 3) {
                        putlog("daemon unreachable for a while; giving up "
                               "grab and exiting");
                        cleanup_and_exit(2);
                    }
                } else {
                    health_fails = 0;
                }
            }
        }

        int timeout = 200;
        for (int i = 0; i < dev_count; i++) {
            pfds[i].fd = devs[i].fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        int prc = poll(pfds, (nfds_t)dev_count, timeout);
        if (prc < 0) {
            if (errno == EINTR) continue;
            putlog("poll error: %s", strerror(errno));
            break;
        }
        if (prc == 0) continue;

        for (int i = 0; i < dev_count; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct input_event ev;
            for (;;) {
                ssize_t n = read(devs[i].fd, &ev, sizeof(ev));
                if (n == (ssize_t)sizeof(ev)) {
                    handle_event(&ev);
                    continue;
                }
                if (n < 0 && errno == EAGAIN) break;
                if (n < 0 && errno == EINTR) continue;
                break;
            }
        }
    }

    putlog("exiting");
    cleanup_and_exit(0);
    return 0;
}