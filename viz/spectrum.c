/*
 * extra-type-viz — native Wayland voice spectrum visualizer.
 *
 * A small GTK4 program that shows a live frequency spectrum (equalizer bars)
 * as a Wayland layer-shell overlay at the bottom of the screen while the
 * extra-type daemon is dictating. It reads the microphone through native
 * PipeWire and computes the FFT itself, so there are no heavy external
 * dependencies (no EasyEffects, no cava) — just GTK4, gtk4-layer-shell and
 * libpipewire, all standard on a modern Wayland desktop.
 *
 * Usage: extra-type-viz [--source NAME] [--bars N] [--listen] [--once]
 *   --listen   keep running even when extra-type asks to stop (diagnostics)
 *   --once     exit after the first audio buffer (diagnostics)
 *
 * The daemon starts/stops this process by forking it; it reads stdin for
 * control, and kills it on stop. Exit codes: 0 = requested stop.
 */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/ringbuffer.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
#define NBARS        5
#define DEFAULT_BARS NBARS
#define DEFAULT_PORT 3232
#define DEFAULT_FFT    512
#define DEFAULT_NCH    1
#define DEFAULT_RATE   48000

static int g_bars = DEFAULT_BARS;
static int g_listen = 0;
static int g_once = 0;
static volatile sig_atomic_t g_quit = 0;

/* ------------------------------------------------------------------ */
/* Naive radix-2 iterative FFT (real input, magnitude output)          */
/* ------------------------------------------------------------------ */
static void fft_real_mag(const float *in, int n, float *mag) {
    double *re = calloc(n, sizeof(double));
    double *im = calloc(n, sizeof(double));
    if (!re || !im) { free(re); free(im); memset(mag, 0, n * sizeof(float)); return; }

    memcpy(re, in, n * sizeof(float));

    int m = 1;
    while (m < n) m <<= 1;
    if (m != n) { /* force power of two */ free(re); free(im); memset(mag,0,n*sizeof(float)); return; }

    /* bit reversal */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cur_r = 1.0, cur_i = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k;
                int b = i + k + len / 2;
                double ar = re[a], ai = im[a];
                double br = re[b]*cur_r - im[b]*cur_i;
                double bi = re[b]*cur_i + im[b]*cur_r;
                re[a] = ar + br; im[a] = ai + bi;
                re[b] = ar - br; im[b] = ai - bi;
                double nr = cur_r*wr - cur_i*wi;
                double ni = cur_r*wi + cur_i*wr;
                cur_r = nr; cur_i = ni;
            }
        }
    }

    /* magnitude from first half */
    for (int i = 0; i < n / 2; i++) {
        mag[i] = (float)sqrt(re[i]*re[i] + im[i]*im[i]);
    }
    free(re); free(im);
}

/* ------------------------------------------------------------------ */
/* Global audio/UI state                                               */
/* ------------------------------------------------------------------ */
static struct {
    float *ring;        /* latest FFT window, float mono */
    int    ring_len;
    int    filled;      /* samples accumulated but not yet FFT'ed */
    float *mag;         /* magnitude spectrum (n/2) */
    float *bars;        /* smoothed bar levels (0..1) */
    int    nbins;
    int    smoothing;   /* exponential smoothing coeff in % */
    double  rms;        /* last RMS energy */
    float   rms_floor;  /* noise floor for activity gate */
    float   run_peak;   /* running peak of FFT magnitude for auto-gain */
    int    active;      /* currently "speaking" flag for idle dim */
    float  speak;       /* smoothed 0..1 energy for smooth idle->wave fade */
    int    idle_ticks;  /* frames since last active */
} g_audio;

/* state mirrored from the daemon's viz-state.json */
static struct {
    int    listening;
    int    loading;
    int    error;
    int    pill;
    char   lang[16];
    int    tick;
    char   status[16];
    char   message[256];
} g_viz;

static GtkWidget *g_window = NULL;
static GtkApplication *g_app = NULL;
static GtkWidget *g_draw = NULL;
static gboolean g_paused = FALSE;
static gboolean g_was_listening = -1; /* -1 = unknown until first poll */
static gboolean g_was_error = -1;     /* -1 = unknown until first poll */

/* pill/transcript overlay */
static int g_port = DEFAULT_PORT;
static int g_had_pill = -1;             /* -1 = unknown until first poll */
static GtkWidget *g_pill_box = NULL;
static GtkWidget *g_pill_scroll = NULL;
static GtkTextView *g_textview = NULL;
static GtkTextBuffer *g_textbuf = NULL;
static GtkWidget *g_pill_caption = NULL;
static gboolean g_pill_sync = FALSE;      /* TRUE while applying daemon text */
static guint g_pill_update_id = 0;        /* debounce source for /pill/update */
static char g_pill_last_sent[65536] = "";

/* read chain: pipewire writes into ring; UI timer FFTs the oldest window */
static void ui_tick(GtkWidget *w, gpointer data);
static void viz_activate(GtkApplication *app);

/* ------------------------------------------------------------------ */
/* Pill overlay                                                        */
/* ------------------------------------------------------------------ */
#define PILL_MAX 65500

/* ------------------------------------------------------------------ */
/* PipeWire                                                             */
/* ------------------------------------------------------------------ */
static struct pw_main_loop *g_loop = NULL;
static struct pw_stream *g_stream = NULL;
static struct spa_hook g_stream_listener;
static struct pw_thread_loop *g_thr_loop = NULL;
static char g_source[129] = "";
static int g_rate = DEFAULT_RATE;
static int g_nch = DEFAULT_NCH;

static void on_process(void *data) {
    struct pw_buffer *b = pw_stream_dequeue_buffer(g_stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    if (!buf->datas || !buf->datas[0].data) {
        pw_stream_queue_buffer(g_stream, b);
        return;
    }
    float *samples = buf->datas[0].data;
    uint32_t nbytes = buf->datas[0].chunk->size;
    uint32_t nframes = nbytes / (g_nch * sizeof(float));

    /* mix channels -> mono into ring buffer, ring holds one FFT window */
    int k = 0;
    for (uint32_t i = 0; i < nframes && g_audio.filled < g_audio.ring_len; i++) {
        float acc = 0;
        for (int c = 0; c < g_nch; c++) acc += samples[i*g_nch + c];
        g_audio.ring[g_audio.filled++] = acc / g_nch;
        (void)k;
    }
    pw_stream_queue_buffer(g_stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static gboolean build_pipewire(void) {
    if (!getenv("SPA_PLUGIN_DIR"))
        setenv("SPA_PLUGIN_DIR", "/usr/lib/spa-0.2", 0);
    if (!getenv("PIPEWIRE_MODULE_DIR"))
        setenv("PIPEWIRE_MODULE_DIR", "/usr/lib/pipewire-0.3", 0);
    pw_init(NULL, NULL);
    g_thr_loop = pw_thread_loop_new("extra-type-viz", NULL);
    if (!g_thr_loop) return FALSE;

    struct pw_properties *cprops = pw_properties_new(
        "context.spa-libs",
        "support.*=support/libspa-support\n"
        "audio.convert.*=audioconvert/libspa-audioconvert\n"
        "api.alsa.*=alsa/libspa-alsa",
        NULL);
    struct pw_context *ctx = pw_context_new(pw_thread_loop_get_loop(g_thr_loop), cprops, 0);
    if (!ctx) return FALSE;
    struct pw_core *core = pw_context_connect(ctx, NULL, 0);
    if (!core) return FALSE;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_NODE_NAME, "extra-type-viz",
        PW_KEY_NODE_DESCRIPTION, "extra-type voice spectrum",
        NULL);
    if (g_source[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, g_source);
    }
    g_stream = pw_stream_new(core, "extra-type-viz", props);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32_LE,
            .channels = g_nch,
            .rate = g_rate));
    pw_stream_add_listener(g_stream, &g_stream_listener, &stream_events, NULL);
    pw_stream_connect(g_stream, PW_DIRECTION_INPUT,
        PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
        params, 1);

    int r = pw_thread_loop_start(g_thr_loop);
    return r == 0;
}

static void teardown_pipewire(void) {
    if (g_thr_loop) {
        pw_thread_loop_stop(g_thr_loop);
        pw_thread_loop_destroy(g_thr_loop);
    }
    if (g_stream) pw_stream_destroy(g_stream);
    pw_deinit();
}

/* ------------------------------------------------------------------ */
/* GTK / layer-shell                                                    */
/* ------------------------------------------------------------------ */
#define BAR_W_DEFAULT 22
#define BAR_GAP 6
#define VIZ_W 160
#define VIZ_H 48
#define LAYER_MARGIN_BOTTOM 6

/* rounded rect (capsule) path helper; (x,y) top-left, w,h box, r corner radius */
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double rc = r < h / 2.0 ? r : h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rc, y + rc, rc, -G_PI / 2, 0);
    cairo_arc(cr, x + w - rc, y + h - rc, rc, 0, G_PI / 2);
    cairo_arc(cr, x + rc, y + h - rc, rc, G_PI / 2, G_PI);
    cairo_arc(cr, x + rc, y + rc, rc, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int wt, int h, gpointer data) {
    (void)area; (void)data;
    int W = wt, H = h;

    /* clear (transparent) */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    /* compact rounded-full dark pill */
    double pr = H / 2.0;
    rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, pr);
    cairo_set_source_rgba(cr, 0.09, 0.10, 0.12, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_stroke(cr);

    int tick = g_viz.tick;
    double pulse = 0.5 + 0.5 * sin((double)tick * 0.35);

    /* accent colour by state: favour the rich status string, fall back to the
     * legacy boolean flags when status is unexpectedly empty */
    double r, g, b;
    int use = 0; /* 0 listening, 1 starting/recovering, 2 error, 3 offline */
    if (g_viz.status[0]) {
        if (strcmp(g_viz.status, "error") == 0) use = 2;
        else if (strcmp(g_viz.status, "offline") == 0) use = 3;
        else if (strcmp(g_viz.status, "starting") == 0 || strcmp(g_viz.status, "recovering") == 0) use = 1;
        else use = 0;
    } else if (g_viz.error) {
        use = 2;
    } else if (g_viz.loading) {
        use = 1;
    }
    if (use == 2) {
        r = 0.96; g = 0.36; b = 0.34;           /* red = error */
    } else if (use == 3) {
        r = 0.96; g = 0.36; b = 0.34;           /* red = offline */
    } else if (use == 1) {
        r = 0.98; g = 0.78; b = 0.30;           /* amber = starting/recovering */
    } else {
        r = 0.45; g = 0.90; b = 0.72;           /* teal = normal */
    }

    /* local language label */
    char label[8] = "";
    if (g_viz.lang[0]) {
        if (strncmp(g_viz.lang, "ru", 2) == 0) snprintf(label, sizeof(label), "RU");
        else if (strncmp(g_viz.lang, "en", 2) == 0) snprintf(label, sizeof(label), "EN");
        else snprintf(label, sizeof(label), "%.2s", g_viz.lang);
    }

    /* metrics */
    double bw = 8.0;
    double gap = 6.0;
    double barsW = NBARS * bw + (NBARS - 1) * gap;

    /* measure the label so the whole cluster can be centred */
    double lw = 0.0;
    if (label[0]) {
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te;
        cairo_text_extents(cr, label, &te);
        lw = te.x_advance;
    }
    double label_bars_gap = label[0] ? 10.0 : 0.0;
    /* centre the whole cluster (label + bars) so the pill has equal space
     * on both sides — no reserved area for a right-side status dot */
    double startX = (W - (lw + label_bars_gap + barsW)) / 2.0;

    double cy = H / 2.0;
    double maxHalf = 12.0;                   /* max bar half-height (shorter) */
    double dim = g_audio.active ? 1.0 : 0.5;

    /* label */
    if (label[0]) {
        cairo_set_source_rgba(cr, r, g, b, use == 1 ? 0.55 + 0.45 * pulse : 0.8);
        cairo_move_to(cr, startX, cy + 4.0);
        cairo_show_text(cr, label);
    }

    /* bars */
    double barsX = startX + lw + label_bars_gap;
    int listening = use == 0;
    for (int i = 0; i < NBARS; i++) {
        float lv = g_audio.bars[i];
        if (lv < 0) lv = 0;
        if (lv > 1) lv = 1;
        double boost;
        if (use == 1) {
            double phase = tick * 0.06;
            boost = 0.6 + 0.4 * (0.5 + 0.5 * sin(phase - i * 1.1));
            dim = 1.0;
        } else if (use == 2 || use == 3) {
            boost = 0.55 + 0.3 * (1.0 - pulse);
        } else if (listening) {
            /* smooth blend between silent idle circles and speaking wave.
             * g_audio.speak (0..1) rises/falls smoothly, so the transition is
             * animated rather than instant. */
            (void)lv;
            double wave = 0.5 + 0.5 * sin(tick * 0.10 - i * 1.1);
            double boost_wave = 0.15 + 0.85 * wave * wave;
            double s = g_audio.speak;
            boost = boost_wave * s;                    /* idle (s=0) keeps small circle */
            dim = 0.35 + (1.0 - 0.35) * s;             /* brighten as you speak */
        } else {
            (void)lv;
            boost = 0.0;
            dim = 0.35;
        }
        double bh = 8.0 + boost * (2.0 * maxHalf - 8.0);   /* total height, centred on cy */
        double bx = barsX + i * (bw + gap);
        rounded_rect(cr, bx, cy - bh / 2.0, bw, bh, bw / 2.0);
        cairo_set_source_rgba(cr, r, g, b, use == 1 ? 0.6 + 0.4 * pulse : 0.95 * dim);
        cairo_fill(cr);
    }

    /* status indicator in the right corner of the pill, drawn as cairo
     * primitives (never a text glyph, so it cannot render as a tofu box):
     *   listening   → filled dot
     *   starting/   → throbbing hollow ring
     *     recovering
     *   error       → filled dot + white "!" bar
     *   offline     → dot with a diagonal strike line
     * Positioned inside the pill with a margin so it never touches the
     * rounded corners. */
    if (g_viz.status[0] || g_viz.listening || (use != 0)) {
        double dx = W - 14.0;
        double dy = cy;
        double rad = 5.0;
        cairo_set_line_width(cr, 2.0);
        if (use == 2) {
            /* error: solid red dot + white exclamation */
            cairo_arc(cr, dx, dy, rad, 0, 2 * G_PI);
            cairo_set_source_rgba(cr, 0.96, 0.36, 0.34, 1.0);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, dx, dy - 3.0);
            cairo_line_to(cr, dx, dy - 0.6);
            cairo_stroke(cr);
            cairo_arc(cr, dx, dy + 2.0, 0.9, 0, 2 * G_PI);
            cairo_fill(cr);
        } else if (use == 3) {
            /* offline: ring + diagonal strike */
            cairo_arc(cr, dx, dy, rad, 0, 2 * G_PI);
            cairo_set_source_rgba(cr, r, g, b, 0.9);
            cairo_stroke(cr);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, dx - rad * 0.7, dy - rad * 0.7);
            cairo_line_to(cr, dx + rad * 0.7, dy + rad * 0.7);
            cairo_stroke(cr);
        } else if (use == 1) {
            /* recovering: throbbing ring, visible only on alternating beats */
            if ((tick / 4) % 2 == 0) {
                double pr = rad + 1.5 + 1.5 * pulse;
                cairo_arc(cr, dx, dy, pr, 0, 2 * G_PI);
                cairo_set_source_rgba(cr, r, g, b, 0.7 + 0.3 * pulse);
                cairo_stroke(cr);
            }
        }
    }
}

/* extract a JSON string value "key":"..." unescaping \" \\ \n \t \r */
static void extract_json_string(const char *buf, const char *key, char *out, size_t outsz) {
    out[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    char *p = strstr(buf, needle);
    if (!p) return;
    p += strlen(needle);
    size_t o = 0;
    while (*p && *p != '"' && o < outsz - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case '"': out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case 'u': /* \uXXXX: leave as literal, best effort */
                if (p[1] && p[2] && p[3] && p[4]) {
                    out[o++] = '?';
                    p += 4;
                }
                break;
            default:  out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

/* minimal HTTP/1.1 JSON POST to the daemon (blocking; used for debounced
 * /pill/update from the user-edited transcript overlay) */
static void http_post_json(const char *path, const char *json_body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(fd);
        return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    size_t body_len = strlen(json_body);
    size_t head_len = (size_t)snprintf(NULL, 0,
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, body_len);
    size_t req_len = head_len + body_len;
    char *req = malloc(req_len + 1);
    if (!req) {
        close(fd);
        return;
    }
    (void)snprintf(req, head_len + 1,
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, body_len);
    memcpy(req + head_len, json_body, body_len);
    req[req_len] = '\0';
    ssize_t w = send(fd, req, req_len, MSG_NOSIGNAL);
    (void)w;
    free(req);
    close(fd);
}

/* JSON-string-escape into a buffer big enough for the transcript */
static size_t json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (; *in && o + 6 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        default:
            if (c < 0x20) {
                out[o] = '\0';
                return o; /* abort rather than emit a broken body */
            }
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

/* debounced /pill/update: called shortly after the user edits the transcript */
static gboolean pill_update_flush(gpointer data) {
    (void)data;
    g_pill_update_id = 0;
    if (!g_textbuf) return G_SOURCE_REMOVE;

    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(g_textbuf, &start);
    gtk_text_buffer_get_end_iter(g_textbuf, &end);
    char *cur = gtk_text_buffer_get_text(g_textbuf, &start, &end, FALSE);

    if (strcmp(cur, g_pill_last_sent) != 0) {
        char body[70000];
        size_t body_sz = sizeof(body);
        snprintf(body, body_sz, "{\"text\":\"");
        char esc[sizeof(body) - 16];
        json_escape(cur, esc, sizeof(esc));
        strncat(body, esc, sizeof(body) - strlen(body) - 3);
        strncat(body, "\"}", 3);
        http_post_json("/pill/update", body);
        snprintf(g_pill_last_sent, sizeof(g_pill_last_sent), "%s", cur);
    }
    g_free(cur);
    return G_SOURCE_REMOVE;
}

static void on_pill_changed(GtkTextBuffer *buf, gpointer data) {
    (void)buf; (void)data;
    if (g_pill_sync) return; /* our own set_text, not a user edit */
    if (!g_textbuf) return;
    if (g_pill_update_id) g_source_remove(g_pill_update_id);
    g_pill_update_id = g_timeout_add(250, pill_update_flush, NULL);
}

/* poll the daemon's viz-state.json; lighter-weight strstr scanning */
static void poll_viz_state(void) {
    const char *home = getenv("HOME");
    const char *state = getenv("XDG_STATE_HOME");
    char path[512];
    if (state && state[0])
        snprintf(path, sizeof(path), "%s/extra-type/viz-state.json", state);
    else
        snprintf(path, sizeof(path), "%s/.local/state/extra-type/viz-state.json", home ? home : "/tmp");

    FILE *f = fopen(path, "r");
    if (!f) {
        g_viz.listening = 0;
        g_viz.loading = 0;
        g_viz.error = 0;
        g_viz.pill = 0;
        g_viz.status[0] = '\0';
        g_viz.message[0] = '\0';
        return;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    g_viz.listening      = strstr(buf, "\"listening\":true") ? 1 : 0;
    g_viz.loading        = strstr(buf, "\"loading\":true") ? 1 : 0;
    g_viz.error          = (strstr(buf, "\"error\":null") == NULL && strstr(buf, "\"error\":\"") != NULL) ? 1 : 0;
    g_viz.pill           = strstr(buf, "\"pill\":true") ? 1 : 0;

    extract_json_string(buf, "status", g_viz.status, sizeof(g_viz.status));
    extract_json_string(buf, "message", g_viz.message, sizeof(g_viz.message));

    /* extract lang like "ru-RU" */
    g_viz.lang[0] = '\0';
    char *p = strstr(buf, "\"lang\":\"");
    if (p) {
        p += 8;
        char *q = strchr(p, '"');
        if (q && (size_t)(q - p) < (int)sizeof(g_viz.lang) - 1) {
            int len = (int)(q - p);
            memcpy(g_viz.lang, p, len);
            g_viz.lang[len] = '\0';
        }
    }

    /* extract dictation text */
    char tmp[65536];
    extract_json_string(buf, "dictationText", tmp, sizeof(tmp));

    /* Apply the daemon text ONLY when the state file actually changed since
     * the last poll. Comparing against the buffer would also fire on user
     * edits (typing/deleting) and clobber them back to the state text on the
     * next tick, making the overlay uneditable. g_pill_last_sent tracks the
     * newest text observed in the state (daemon→viz or our own pushed edit),
     * so untouched dictation keeps flowing while a hand edit survives a poll
     * where the state did not move. */
    if (g_textbuf) {
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(g_textbuf, &start);
        gtk_text_buffer_get_end_iter(g_textbuf, &end);
        char *cur = gtk_text_buffer_get_text(g_textbuf, &start, &end, FALSE);
        int changed = strcmp(tmp, g_pill_last_sent) != 0;
        int differs = strcmp(tmp, cur) != 0;
        if (changed && differs) {
            g_pill_sync = TRUE;
            gtk_text_buffer_set_text(g_textbuf, tmp, -1);
            g_pill_sync = FALSE;
            snprintf(g_pill_last_sent, sizeof(g_pill_last_sent), "%s", tmp);
            GtkTextMark *mark = gtk_text_buffer_get_insert(g_textbuf);
            if (g_textview) gtk_text_view_scroll_mark_onscreen(g_textview, mark);
        }
        g_free(cur);
    }
}

static gboolean ui_tick_cb(gpointer data) {
    if (g_quit) {
        if (g_app) g_application_quit(G_APPLICATION(g_app));
        return G_SOURCE_REMOVE;
    }
    ui_tick(data, NULL);
    return G_SOURCE_CONTINUE;
}

/* called on gtk main loop; pull oldest FFT window, compute bars */
static void ui_tick(GtkWidget *w, gpointer data) {
    (void)w; (void)data;

    int n = g_audio.ring_len;
    if (g_audio.filled >= n) {
        /* compute FFT on last n samples */
        float *win = malloc(n * sizeof(float));
        memcpy(win, g_audio.ring + g_audio.filled - n, n * sizeof(float));

        /* RMS */
        double sum = 0;
        for (int i = 0; i < n; i++) sum += win[i]*win[i];
        double rms = sqrt(sum / n);
        g_audio.rms = rms;
        g_audio.active = rms > g_audio.rms_floor;
        if (g_audio.active) g_audio.idle_ticks = 0;
        else if (g_audio.idle_ticks < 1000000) g_audio.idle_ticks++;
        /* smoothed activity level -> smooth idle/wave transition */
        float speak_target = g_audio.active ? 1.0f : 0.0f;
        g_audio.speak += (speak_target - g_audio.speak) * 0.12f;
        if (g_audio.speak < 0.001f) g_audio.speak = 0.0f;

        fft_real_mag(win, n, g_audio.mag);
        free(win);

        /* map log-frequency bins to bars */
        int nbins = g_audio.nbins;
        int half = n / 2;
        double min_log = log(70.0);
        double max_log = log(6000.0);
        for (int b = 0; b < nbins; b++) {
            double lo = (double)b / nbins;
            double hi = (double)(b + 1) / nbins;
            double fl = exp(min_log + lo * (max_log - min_log));
            double fh = exp(min_log + hi * (max_log - min_log));
            int il = (int)(fl * n / (double)g_rate);
            int ih = (int)(fh * n / (double)g_rate);
            if (il < 0) il = 0;
            if (ih >= half) ih = half - 1;
            if (il > ih) ih = il;
            float peak = 0;
            for (int j = il; j <= ih; j++) peak = peak > g_audio.mag[j] ? peak : g_audio.mag[j];
            g_audio.bars[b] = peak;
        }

        /* auto-gain: normalise each window by its own loudest bin so the bars
         * actually rise while dictating instead of flattening to the idle size */
        float win_max = 0;
        for (int b = 0; b < nbins; b++) if (g_audio.bars[b] > win_max) win_max = g_audio.bars[b];
        if (win_max > g_audio.run_peak * 1.5f) g_audio.run_peak = win_max;      /* fast rise */
        else g_audio.run_peak = g_audio.run_peak * 0.90f + win_max * 0.10f;     /* slow decay */
        if (g_audio.run_peak < 512.0f) g_audio.run_peak = 512.0f;               /* min floor */

        float smooth = g_audio.smoothing / 100.0f;
        for (int b = 0; b < nbins; b++) {
            float idi = g_audio.bars[b] / g_audio.run_peak * 0.92f;   /* 0..~1, headroom */
            if (idi > 1.0f) idi = 1.0f;
            if (idi < 0.0f) idi = 0.0f;
            g_audio.bars[b] = g_audio.bars[b] * smooth + idi * (1.0f - smooth);
        }

        g_audio.filled = 0; /* start next window fresh */
    }

    if ((++g_viz.tick % 6) == 0)
        poll_viz_state();

    /* the pill exists only while the daemon is listening: show on start of
     * dictation, hide when it sleeps (or the daemon/state file goes away).
     * In overlay mode (pill) the transcript panel is shown instead of the
     * spectrum and only while dictating — after stop the text goes to the
     * clipboard and the overlay hides. */
    int overlay_on = g_viz.pill && g_viz.listening;
    /* a fatal error while dictating must keep the pill visible (red status)
     * until the daemon clears state — don't hide it on listening=false */
    int err_state = g_viz.status[0] && (strcmp(g_viz.status, "error") == 0 || strcmp(g_viz.status, "offline") == 0);
    if (g_was_listening != g_viz.listening || g_had_pill != g_viz.pill || (err_state && g_was_error != 1)) {
        g_was_listening = g_viz.listening;
        g_had_pill = g_viz.pill;
        g_was_error = err_state ? 1 : 0;
        gboolean now_show = g_viz.listening || overlay_on || err_state;
        /* set the size BEFORE showing to avoid a transparent first frame at
         * the wrong dimensions — gtk4-layer-shell re-maps the surface on
         * resize, so showing at VIZ_W×VIZ_H and then resizing to 560×220
         * leaves a ghost transparent frame until the compositor acks the
         * new size */
        if (g_viz.pill) {
            gtk_widget_set_size_request(g_window, 560, 220);
        } else {
            gtk_widget_set_size_request(g_window, VIZ_W, VIZ_H);
        }
        gtk_widget_set_visible(g_window, now_show);
        /* only the overlay (pill) mode lets the layer ask for the keyboard,
         * and only on demand; type/paste/dotool keep it NONE so the layer
         * never steals focus from the window the user is typing into */
        gtk_layer_set_keyboard_mode(GTK_WINDOW(g_window),
            g_viz.pill ? GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND
                       : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    }

    /* spectrum above the transcript is hidden in overlay mode */
    if (g_pill_box) {
        gboolean want_pill_visible = overlay_on;
        gboolean pill_visible = gtk_widget_get_visible(g_pill_box);
        if (pill_visible != want_pill_visible)
            gtk_widget_set_visible(g_pill_box, want_pill_visible);
        gtk_widget_set_visible(g_draw, !want_pill_visible);
    }

    /* live status line under the transcription (overlay/pill mode) */
    if (g_pill_caption && g_pill_box && overlay_on) {
        const char *txt = NULL;
        int error_class = 0;
        if (g_viz.status[0] && strcmp(g_viz.status, "error") == 0) {
            txt = "Error: ";
            error_class = 1;
        } else if (g_viz.status[0] && strcmp(g_viz.status, "offline") == 0) {
            txt = "Offline — recognition unavailable";
        } else if (g_viz.status[0] &&
                   (strcmp(g_viz.status, "starting") == 0 || strcmp(g_viz.status, "recovering") == 0)) {
            txt = "Recovering… / Please wait";
        }
        const char *cur = gtk_label_get_text(GTK_LABEL(g_pill_caption));
        char full[320];
        if (txt) {
            if (error_class) {
                if (g_viz.message[0])
                    snprintf(full, sizeof(full), "Error: %s", g_viz.message);
                else
                    snprintf(full, sizeof(full), "Error");
                gtk_widget_add_css_class(g_pill_caption, "pill-status-error");
            } else {
                snprintf(full, sizeof(full), "%s", txt);
                gtk_widget_remove_css_class(g_pill_caption, "pill-status-error");
            }
        } else {
            snprintf(full, sizeof(full), "Turn off dictation to copy the text");
            gtk_widget_remove_css_class(g_pill_caption, "pill-status-error");
        }
        /* only touch the label when the text actually changed to avoid
         * needless redraws every tick */
        if (strcmp(cur, full) != 0)
            gtk_label_set_text(GTK_LABEL(g_pill_caption), full);
    }

    /* redraw the right target every tick: in pill mode g_draw is hidden
     * and the pill_box lives under g_window so queue on the window;
     * in spectrum mode g_draw is the DrawingArea whose on_draw callback
     * paints the bars — queue on it, not the window */
    if (g_viz.pill)
        gtk_widget_queue_draw(g_window);
    else
        gtk_widget_queue_draw(GTK_WIDGET(g_draw));
    if (g_once) exit(0);
}

/* ------------------------------------------------------------------ */
/* CLI + signal                                                        */
/* ------------------------------------------------------------------ */
static void on_sigint(int sig) {
    (void)sig;
    g_quit = 1;
    if (g_thr_loop) pw_thread_loop_signal(g_thr_loop, FALSE);
    if (g_loop) pw_main_loop_quit(g_loop);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --source NAME   PipeWire source node name to capture (default: default input)\n"
        "  --bars N        number of spectrum bars (default %d)\n"
        "  --port N        daemon HTTP port for the transcript overlay (default %d)\n"
        "  --listen        keep running until stdin EOF (daemon mode)\n"
        "  --once          exit after one FFT window (diagnostics)\n"
        "  --help          show this help\n",
        prog, DEFAULT_BARS, DEFAULT_PORT);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            snprintf(g_source, sizeof(g_source), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bars") == 0 && i + 1 < argc) {
            g_bars = atoi(argv[++i]);
            if (g_bars < 1) g_bars = 1;
            if (g_bars > 64) g_bars = 64;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            if (g_port < 1 || g_port > 65536) g_port = DEFAULT_PORT;
        } else if (strcmp(argv[i], "--listen") == 0) {
            g_listen = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            g_once = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    signal(SIGTERM, on_sigint);
    signal(SIGINT, on_sigint);

    g_audio.nbins = g_bars;
    g_audio.active = 1; /* assume listening until first real rms */
    g_audio.smoothing = 55;
    g_audio.rms_floor = 0.0008f;
    g_audio.run_peak = 512.0f;
    g_audio.speak = 0.0f;

    g_audio.ring_len = DEFAULT_FFT;
    g_audio.ring = calloc(g_audio.ring_len, sizeof(float));
    g_audio.mag = calloc(DEFAULT_FFT / 2, sizeof(float));
    g_audio.bars = calloc(g_bars, sizeof(float));
    if (!g_audio.ring || !g_audio.mag || !g_audio.bars) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    GtkApplication *app = gtk_application_new("org.extra.type.viz", 0);
    g_app = app;
    g_signal_connect(app, "activate", G_CALLBACK(viz_activate), NULL);
    /* --listen/--once are our own options; GApplication would reject them,
     * so hand it a clean argv of just the program name when they are set. */
    char *clean_argv[] = { argv[0] ? argv[0] : "extra-type-viz", NULL };
    int n_run = (g_listen || g_once) ? 1 : argc;
    char **run_argv = (g_listen || g_once) ? clean_argv : argv;
    int status = g_application_run(G_APPLICATION(app), n_run, run_argv);
    g_object_unref(app);

    teardown_pipewire();
    free(g_audio.ring);
    free(g_audio.mag);
    free(g_audio.bars);
    return status;
}

static void viz_activate(GtkApplication *app) {
    if (!getenv("SPA_PLUGIN_DIR"))
        setenv("SPA_PLUGIN_DIR", "/usr/lib/spa-0.2", 0);
    if (!getenv("PIPEWIRE_MODULE_DIR"))
        setenv("PIPEWIRE_MODULE_DIR", "/usr/lib/pipewire-0.3", 0);
    pw_init(NULL, NULL);
    g_loop = pw_main_loop_new(NULL);
    if (!build_pipewire()) {
        fprintf(stderr, "Failed to connect to PipeWire (is a compositor/audio running?)\n");
        exit(1);
    }

    /* make the layer surface transparent so the pill's rounded corners show
     * against the desktop instead of an opaque rectangular backdrop */
    GdkDisplay *disp = gdk_display_get_default();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background-color: transparent; }\n"
        "drawingarea { background: transparent; }\n"
        "#pillbox { background-color: rgba(22, 24, 30, 1.0); border-radius: 8px; border: 1px solid rgba(255,255,255,0.15); }\n"
        "#pillbox scrolledwindow { background-color: transparent; }\n"
        "#pillbox textview { background-color: transparent; }\n"
        "#pillbox text { color: #e6e6e6; }\n"
        "#pillbox textview text selection { background-color: alpha(@theme_selected_bg_color, 0.4); color: #ffffff; }\n"
        "#pillbox textview caret { color: #ffffff; }\n"
        ".pill-caption { color: alpha(@theme_fg_color, 0.6); font-size: 11pt; }\n"
        ".pill-status-error { color: #f05a4d; font-size: 11pt; font-weight: bold; }\n");
    gtk_style_context_add_provider_for_display(disp,
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_window = gtk_application_window_new(app);
    gtk_layer_init_for_window(GTK_WINDOW(g_window));
    gtk_layer_set_layer(GTK_WINDOW(g_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(g_window), GTK_LAYER_SHELL_EDGE_BOTTOM, LAYER_MARGIN_BOTTOM);
    /* The layer only interacts with the keyboard in overlay (pill) mode, and
     * only then on demand (click) so the user can edit the transcript. In
     * type/paste/dotool modes keyboard mode stays NONE so showing/hiding the
     * indicator pill never steals focus from the dictation target window. */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(g_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(g_window), 0);

    gtk_widget_set_size_request(g_window, VIZ_W, VIZ_H);
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);

    g_draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_draw, VIZ_W, VIZ_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_draw), on_draw, NULL, NULL);

    /* root box: spectrum on top, transcript overlay below */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_draw);
    gtk_widget_set_halign(g_draw, GTK_ALIGN_START);

    /* transcript overlay (only in pill mode) */
    g_pill_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(g_pill_box, "pillbox");
    gtk_widget_set_visible(g_pill_box, FALSE);
    gtk_widget_set_hexpand(g_pill_box, TRUE);
    gtk_widget_set_halign(g_pill_box, GTK_ALIGN_FILL);

    g_textview = GTK_TEXT_VIEW(gtk_text_view_new());
    g_textbuf = gtk_text_view_get_buffer(g_textview);
    g_signal_connect(g_textbuf, "changed", G_CALLBACK(on_pill_changed), NULL);
    gtk_text_view_set_editable(g_textview, TRUE);
    gtk_text_view_set_cursor_visible(g_textview, TRUE);
    gtk_text_view_set_wrap_mode(g_textview, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(g_textview, TRUE);
    gtk_text_view_set_left_margin(g_textview, 14);
    gtk_text_view_set_right_margin(g_textview, 14);
    gtk_text_view_set_top_margin(g_textview, 12);
    gtk_text_view_set_bottom_margin(g_textview, 12);
    gtk_widget_set_size_request(GTK_WIDGET(g_textview), 540, 128);

    g_pill_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_pill_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(g_pill_scroll), GTK_WIDGET(g_textview));
    gtk_widget_set_size_request(g_pill_scroll, 540, 128);
    gtk_box_append(GTK_BOX(g_pill_box), g_pill_scroll);

    /* hint the user the transcript is copied on stop */
    GtkWidget *caption = gtk_label_new("Turn off dictation to copy the text");
    g_pill_caption = caption;
    gtk_widget_set_halign(caption, GTK_ALIGN_START);
    gtk_widget_set_margin_top(caption, 4);
    gtk_widget_set_margin_bottom(caption, 4);
    gtk_widget_set_margin_start(caption, 14);
    gtk_widget_add_css_class(caption, "pill-caption");
    gtk_box_append(GTK_BOX(g_pill_box), caption);

    gtk_box_append(GTK_BOX(root), g_pill_box);

    gtk_window_set_child(GTK_WINDOW(g_window), root);

    g_timeout_add(33, ui_tick_cb, g_draw); /* ~30 fps */

    /* start hidden; only shown while the daemon is listening */
    gtk_widget_set_visible(g_window, FALSE);
}
