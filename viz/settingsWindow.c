/*
 * extra-type-settings — GTK4 settings window for the extra-type daemon.
 *
 * Connects to the daemon's HTTP API (localhost:<port>), reads the current
 * config, lets the user change language, autodetect, notifications, etc.,
 * and writes changes back via POST /config.
 *
 * Usage: extra-type-settings [port]
 *   port   daemon port (default 3232)
 */

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#define DEFAULT_PORT 3232

static int g_port = DEFAULT_PORT;

/* ------------------------------------------------------------------ */
/* Languages supported by Google Web Speech API                        */
/* ------------------------------------------------------------------ */
static const char *LANG_CODES[] = {
    "en-US", "en-GB", "en-AU", "en-CA", "en-IN",
    "es-ES", "es-MX", "es-AR", "es-CO",
    "ru-RU",
    "zh-CN", "zh-TW", "zh-HK",
    "ja-JP", "ko-KR",
    "fr-FR", "fr-CA",
    "de-DE", "de-AT", "de-CH",
    "pt-BR", "pt-PT",
    "it-IT", "nl-NL", "pl-PL",
    "tr-TR", "ar-SA", "hi-IN",
    "sv-SE", "no-NO", "da-DK", "fi-FI",
    "el-GR", "he-IL", "th-TH", "vi-VN",
    "id-ID", "uk-UA", "cs-CZ", "ro-RO", "hu-HU",
    NULL
};

static const char *LANG_LABELS[] = {
    "English (US)", "English (UK)", "English (AU)", "English (CA)", "English (IN)",
    "Spanish (ES)", "Spanish (MX)", "Spanish (AR)", "Spanish (CO)",
    "Russian",
    "Chinese (Simplified)", "Chinese (Traditional TW)", "Chinese (Traditional HK)",
    "Japanese", "Korean",
    "French (FR)", "French (CA)",
    "German (DE)", "German (AT)", "German (CH)",
    "Portuguese (BR)", "Portuguese (PT)",
    "Italian", "Dutch", "Polish",
    "Turkish", "Arabic", "Hindi",
    "Swedish", "Norwegian", "Danish", "Finnish",
    "Greek", "Hebrew", "Thai", "Vietnamese",
    "Indonesian", "Ukrainian", "Czech", "Romanian", "Hungarian",
    NULL
};

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */
static char *http_get(const char *path) {
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", g_port, path);

    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) return NULL;

    SoupSession *session = soup_session_new();
    GBytes *bytes = soup_session_send_and_read(session, msg, NULL, NULL);
    g_object_unref(session);

    char *body = NULL;
    if (bytes) {
        gsize len = 0;
        const char *data = g_bytes_get_data(bytes, &len);
        body = g_strndup(data, len);
        g_bytes_unref(bytes);
    }
    g_object_unref(msg);
    return body;
}

static int http_post(const char *path, const char *json_body) {
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", g_port, path);

    SoupMessage *msg = soup_message_new("POST", url);
    if (!msg) return -1;

    GBytes *body_bytes = g_bytes_new(json_body, strlen(json_body));
    soup_message_set_request_body_from_bytes(msg, "application/json", body_bytes);
    g_bytes_unref(body_bytes);

    SoupSession *session = soup_session_new();
    soup_session_send_and_read(session, msg, NULL, NULL);
    guint status = soup_message_get_status(msg);
    g_object_unref(session);
    g_object_unref(msg);
    return (int)status;
}

/* ------------------------------------------------------------------ */
/* JSON parsing                                                         */
/* ------------------------------------------------------------------ */
static char *json_get_string(JsonObject *obj, const char *key) {
    if (!json_object_has_member(obj, key)) return NULL;
    JsonNode *node = json_object_get_member(obj, key);
    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(node));
    return NULL;
}

static gboolean json_get_bool(JsonObject *obj, const char *key, gboolean def) {
    if (!json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
        return json_node_get_boolean(node);
    return def;
}

static int json_get_int(JsonObject *obj, const char *key, int def) {
    if (!json_object_has_member(obj, key)) return def;
    JsonNode *node = json_object_get_member(obj, key);
    if (json_node_get_value_type(node) == G_TYPE_INT64)
        return (int)json_node_get_int(node);
    return def;
}

/* ------------------------------------------------------------------ */
/* Widget references (global for callbacks)                             */
/* ------------------------------------------------------------------ */
static GtkDropDown *g_lang_dropdown = NULL;
static GtkSwitch   *g_autodetect_switch = NULL;
static GtkSwitch   *g_sound_switch = NULL;
static GtkSwitch   *g_stream_switch = NULL;
static GtkSwitch   *g_punctuation_switch = NULL;
static GtkSpinButton *g_timeout_spin = NULL;
static GtkLabel    *g_status_label = NULL;

static char *g_current_lang = NULL;
static gboolean g_loading = FALSE;
static guint g_timeout_src = 0;
static GtkButton *g_hotkey_button = NULL;
static gboolean g_capturing = FALSE;
static gboolean g_capture_paste = FALSE; /* TRUE: capturing paste combo, not hotkey */
static char *g_hotkey_display = NULL;

static GtkDropDown *g_insert_dropdown = NULL;
static GtkButton *g_paste_button = NULL;
static char *g_paste_display = NULL;

static GtkWidget *g_window = NULL; /* single settings window */

static char *wire_to_display(const char *wire);
static void update_hotkey_label(void);
static void update_paste_label(void);

/* ------------------------------------------------------------------ */
/* Load config from daemon                                             */
/* ------------------------------------------------------------------ */
static void load_config(void) {
    char *body = http_get("/config");
    if (!body) {
        gtk_label_set_text(g_status_label, "Cannot connect to daemon");
        return;
    }

    GError *err = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, -1, &err)) {
        gtk_label_set_text(g_status_label, "Invalid JSON from daemon");
        g_free(body);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    g_loading = TRUE;

    /* Language */
    g_free(g_current_lang);
    g_current_lang = json_get_string(obj, "lang");
    if (g_current_lang) {
        for (int i = 0; LANG_CODES[i]; i++) {
            if (strcmp(LANG_CODES[i], g_current_lang) == 0) {
                gtk_drop_down_set_selected(g_lang_dropdown, i);
                break;
            }
        }
    }

    /* Toggles */
    gtk_switch_set_active(g_autodetect_switch,
        json_get_bool(obj, "autodetect_lang", FALSE));
    gtk_switch_set_active(g_sound_switch,
        json_get_bool(obj, "sound", TRUE));
    if (g_stream_switch)
        gtk_switch_set_active(g_stream_switch,
            json_get_bool(obj, "stream", TRUE));
    gtk_switch_set_active(g_punctuation_switch,
        json_get_bool(obj, "punctuation", TRUE));

    /* Timeout */
    gtk_spin_button_set_value(g_timeout_spin,
        json_get_int(obj, "timeout", 0));

    /* Global hotkey */
    g_free(g_hotkey_display);
    g_hotkey_display = NULL;
    char *hk = json_get_string(obj, "hotkey");
    if (hk) {
        g_hotkey_display = wire_to_display(hk);
        g_free(hk);
    }
    update_hotkey_label();

    /* Text insertion */
    char *method = json_get_string(obj, "insert_method");
    if (method && g_insert_dropdown) {
        int idx = 0;
        if (strcmp(method, "paste") == 0) idx = 1;
        else if (strcmp(method, "pill") == 0) idx = 2;
        else if (strcmp(method, "dotool") == 0) idx = 3;
        gtk_drop_down_set_selected(g_insert_dropdown, idx);
        g_free(method);
    }
    g_free(g_paste_display);
    g_paste_display = NULL;
    char *paste = json_get_string(obj, "paste");
    if (paste) {
        g_paste_display = wire_to_display(paste);
        g_free(paste);
    }
    update_paste_label();

    g_loading = FALSE;

    gtk_label_set_text(g_status_label, "");
    g_free(body);
    g_object_unref(parser);
}

/* ------------------------------------------------------------------ */
/* Push config to daemon (immediate save)                              */
/* ------------------------------------------------------------------ */
static void push_config(const char *json) {
    int status = http_post("/config", json);
    if (status == 200) {
        gtk_label_set_text(g_status_label, "");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Save failed (HTTP %d)", status);
        gtk_label_set_text(g_status_label, msg);
    }
}

static void on_lang_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    if (g_loading) return;
    guint idx = gtk_drop_down_get_selected(dd);
    const char *lang = (idx < G_N_ELEMENTS(LANG_CODES) - 1) ? LANG_CODES[idx] : "en-US";
    char *json = g_strdup_printf("{\"lang\":\"%s\"}", lang);
    push_config(json);
    g_free(json);
}

static void on_bool_changed(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    if (g_loading) return;
    const char *key = (const char *)user_data;
    char *json = g_strdup_printf("{\"%s\":%s}", key,
        gtk_switch_get_active(sw) ? "true" : "false");
    push_config(json);
    g_free(json);
}

static void on_insert_changed(GtkDropDown *dd, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    if (g_loading) return;
    guint idx = gtk_drop_down_get_selected(dd);
    const char *method = "type";
    if (idx == 1) method = "paste";
    else if (idx == 2) method = "pill";
    else if (idx == 3) method = "dotool";
    char *json = g_strdup_printf("{\"insert_method\":\"%s\"}", method);
    push_config(json);
    g_free(json);
}

static gboolean push_timeout(gpointer user_data) {
    (void)user_data;
    g_timeout_src = 0;
    int timeout = (int)gtk_spin_button_get_value(g_timeout_spin);
    char *json = g_strdup_printf("{\"timeout\":%d}", timeout);
    push_config(json);
    g_free(json);
    return G_SOURCE_REMOVE;
}

static void on_timeout_changed(GtkSpinButton *spin, GParamSpec *pspec, gpointer user_data) {
    (void)spin; (void)pspec; (void)user_data;
    if (g_loading) return;
    if (g_timeout_src) {
        g_source_remove(g_timeout_src);
        g_timeout_src = 0;
    }
    g_timeout_src = g_timeout_add(400, push_timeout, NULL);
}

/* ------------------------------------------------------------------ */
/* Global hotkey                                                       */
/* ------------------------------------------------------------------ */
static int evdev_offset(void) {
#ifdef GDK_WINDOWING_X11
    GdkDisplay *disp = gdk_display_get_default();
    if (disp && GDK_IS_X11_DISPLAY(disp)) return 8; /* X11 hw keycode = evdev + 8 */
#endif
    return 0;
}

/* Render a stored combo like "ctrl+alt+57" for display. */
static char *wire_to_display(const char *wire) {
    if (!wire || !*wire) return NULL;
    GString *s = g_string_new(NULL);
    char *dup = g_strdup(wire);
    char *save = NULL;
    for (char *tok = strtok_r(dup, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        char *end = NULL;
        long code = strtol(tok, &end, 10);
        if (end && *end == '\0' && code > 0) {
            g_string_append_printf(s, (s->len ? "+[%d]" : "[%d]"), (int)code);
        } else {
            if (s->len) g_string_append_c(s, '+');
            if (strcmp(tok, "ctrl") == 0) g_string_append(s, "Ctrl");
            else if (strcmp(tok, "alt") == 0) g_string_append(s, "Alt");
            else if (strcmp(tok, "super") == 0) g_string_append(s, "Super");
            else if (strcmp(tok, "shift") == 0) g_string_append(s, "Shift");
            else {
                char cap[8];
                snprintf(cap, sizeof(cap), "%c%s", (char)g_ascii_toupper(tok[0]), tok + 1);
                g_string_append(s, cap);
            }
        }
    }
    g_free(dup);
    return g_string_free(s, FALSE);
}

static void update_hotkey_label(void) {
    if (!g_hotkey_button) return;
    gtk_button_set_label(g_hotkey_button, g_hotkey_display ? g_hotkey_display : "Not set");
}

static void update_paste_label(void) {
    if (!g_paste_button) return;
    gtk_button_set_label(g_paste_button, g_paste_display ? g_paste_display : "Not set");
}

static void end_capture(void) {
    g_capturing = FALSE;
    g_capture_paste = FALSE;
    update_hotkey_label();
    update_paste_label();
}

static void begin_capture(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *target = (const char *)user_data;
    g_capturing = TRUE;
    g_capture_paste = (target && strcmp(target, "paste") == 0);
    gtk_button_set_label(g_capture_paste ? g_paste_button : g_hotkey_button,
        "Press keys… (Esc cancels)");
}

static void clear_hotkey(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    end_capture();
    g_free(g_hotkey_display);
    g_hotkey_display = NULL;
    update_hotkey_label();
    push_config("{\"hotkey\":null}");
}

static void clear_paste(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    end_capture();
    g_free(g_paste_display);
    g_paste_display = NULL;
    update_paste_label();
    /* reset to the default combo */
    g_paste_display = g_strdup("Ctrl+V");
    update_paste_label();
    push_config("{\"paste\":\"ctrl+v\"}");
}

static gboolean is_modifier_key(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
    case GDK_KEY_ISO_Level3_Shift:
    case GDK_KEY_ISO_Level5_Shift:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean on_capture_key(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)user_data;
    if (!g_capturing) return GDK_EVENT_PROPAGATE;

    if (keyval == GDK_KEY_Escape) {
        end_capture();
        return GDK_EVENT_STOP;
    }
    if (is_modifier_key(keyval)) return GDK_EVENT_STOP; /* wait for the main key */

    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt = (state & GDK_ALT_MASK) != 0;
    gboolean super = (state & GDK_SUPER_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    const char *kname = gdk_keyval_name(keyval);
    char *wire = NULL;

    if (g_capture_paste) {
        /* Paste combo: named key (wtype/libxkbcommon style), e.g. "ctrl+shift+v". */
        GString *w = g_string_new(NULL);
        if (ctrl) g_string_append(w, "ctrl+");
        if (alt) g_string_append(w, "alt+");
        if (super) g_string_append(w, "super+");
        if (shift) g_string_append(w, "shift+");
        if (kname && kname[0]) {
            if (kname[1] == '\0') g_string_append_c(w, (char)g_ascii_tolower(kname[0]));
            else g_string_append(w, kname);
        } else {
            int code = (int)keycode - evdev_offset();
            g_string_append_printf(w, "%d", code);
        }
        wire = g_strdup(w->str);
        g_string_free(w, TRUE);
    } else {
        int code = (int)keycode - evdev_offset();
        if (code < 1 || code > 767 /* KEY_MAX */) {
            return GDK_EVENT_STOP;
        }
        GString *w = g_string_new(NULL);
        if (ctrl) g_string_append(w, "ctrl+");
        if (alt) g_string_append(w, "alt+");
        if (super) g_string_append(w, "super+");
        if (shift) g_string_append(w, "shift+");
        g_string_append_printf(w, "%d", code);
        wire = g_strdup(w->str);
        g_string_free(w, TRUE);
    }

    GString *lbl = g_string_new(NULL);
    if (ctrl) g_string_append(lbl, "Ctrl+");
    if (alt) g_string_append(lbl, "Alt+");
    if (super) g_string_append(lbl, "Super+");
    if (shift) g_string_append(lbl, "Shift+");
    if (kname) {
        if (kname[0] && g_ascii_isalpha(kname[0]))
            g_string_append_c(lbl, (char)g_ascii_toupper(kname[0]));
        if (kname[1]) g_string_append(lbl, kname + 1);
        else g_string_append_c(lbl, kname[0]);
    } else {
        char *end = NULL;
        long code = strtol(wire, &end, 10);
        if (end && *end == '\0') g_string_append_printf(lbl, "[%d]", (int)code);
    }

    if (g_capture_paste) {
        g_free(g_paste_display);
        g_paste_display = g_strdup(lbl->str);
        char *json = g_strdup_printf("{\"paste\":\"%s\"}", wire);
        push_config(json);
        g_free(json);
    } else {
        g_free(g_hotkey_display);
        g_hotkey_display = g_strdup(lbl->str);
        char *json = g_strdup_printf("{\"hotkey\":\"%s\"}", wire);
        push_config(json);
        g_free(json);
    }

    g_string_free(lbl, TRUE);
    g_free(wire);

    end_capture();
    return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
/* ------------------------------------------------------------------ */
static GtkWidget *make_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    return box;
}

static GtkWidget *make_title(const char *text) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(lbl), attrs);
    pango_attr_list_unref(attrs);
    return lbl;
}

static GtkWidget *make_note(const char *text) {
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "auto-note");
    return lbl;
}

static GtkWidget *make_switch_row(GtkSwitch **out_switch, const char *key,
                                  const char *label) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    *out_switch = GTK_SWITCH(gtk_switch_new());
    /* key is always a static string literal, safe as user_data */
    g_signal_connect(*out_switch, "notify::active", G_CALLBACK(on_bool_changed),
                     (gpointer)key);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(*out_switch));

    GtkWidget *lblw = gtk_label_new(label);
    gtk_label_set_wrap(GTK_LABEL(lblw), TRUE);
    gtk_box_append(GTK_BOX(box), lblw);
    return box;
}

static void on_window_destroyed(GtkWidget *widget, gpointer data) {
    (void)widget;
    GtkWidget **win = (GtkWidget **)data;
    *win = NULL;
}

/* ------------------------------------------------------------------ */
/* Activate                                                            */
/* ------------------------------------------------------------------ */
static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    /* single-instance: a second activation must just raise the existing
     * window instead of creating another one */
    if (g_window) {
        gtk_window_present(GTK_WINDOW(g_window));
        return;
    }

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Extra Type - Settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 540, 560);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE); /* fixed width/height */

    GtkEventController *hk_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(hk_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(hk_ctrl, "key-pressed", G_CALLBACK(on_capture_key), NULL);
    gtk_widget_add_controller(win, hk_ctrl);

    GdkDisplay *disp = gdk_display_get_default();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".auto-note { color: alpha(@theme_fg_color, 0.75); }\n"
        "notebook { border: none; }\n"
        "notebook > header { background: alpha(@theme_bg_color, 0.4); }\n");
    gtk_style_context_add_provider_for_display(disp,
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* root: notebook on top, always-visible status line below */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(nb), FALSE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), FALSE);
    gtk_widget_set_hexpand(nb, TRUE);
    gtk_widget_set_vexpand(nb, TRUE);
    gtk_box_append(GTK_BOX(root), nb);

    /* ================= General ================= */
    GtkWidget *gen = make_page();

    gtk_box_append(GTK_BOX(gen), make_title("Language"));

    GtkStringList *lang_list = gtk_string_list_new(NULL);
    for (int i = 0; LANG_LABELS[i]; i++) {
        char label[64];
        snprintf(label, sizeof(label), "%s  (%s)", LANG_LABELS[i], LANG_CODES[i]);
        gtk_string_list_append(lang_list, label);
    }
    g_lang_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(lang_list), NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(g_lang_dropdown), TRUE);
    g_signal_connect(g_lang_dropdown, "notify::selected", G_CALLBACK(on_lang_changed), NULL);
    gtk_box_append(GTK_BOX(gen), GTK_WIDGET(g_lang_dropdown));

    gtk_box_append(GTK_BOX(gen),
        make_switch_row(&g_stream_switch, "stream", "Live typing while dictating"));
    gtk_box_append(GTK_BOX(gen),
        make_note("<small>Correct only in native Wayland apps; in Chrome and Electron "
                  "apps (Discord) live typing can garble or erase text. "
                  "For reliable input everywhere, turn Live typing off and use "
                  "Paste via clipboard, or use the Overlay mode instead.</small>"));

    gtk_box_append(GTK_BOX(gen), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(gen), make_title("Insertion mode"));

    GtkStringList *ins_list = gtk_string_list_new(NULL);
    gtk_string_list_append(ins_list, "Virtual keyboard - Wayland apps only");
    gtk_string_list_append(ins_list, "Paste via shortcut");
    gtk_string_list_append(ins_list, "Overlay mode - works everywhere");
    gtk_string_list_append(ins_list, "Hardware keyboard - English only");
    g_insert_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(ins_list), NULL));
    gtk_drop_down_set_selected(g_insert_dropdown, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(g_insert_dropdown), TRUE);
    g_signal_connect(g_insert_dropdown, "notify::selected", G_CALLBACK(on_insert_changed), NULL);
    gtk_box_append(GTK_BOX(gen), GTK_WIDGET(g_insert_dropdown));

    GtkWidget *paste_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(gen), paste_box);

    g_paste_button = GTK_BUTTON(gtk_button_new_with_label("Not set"));
    gtk_widget_set_hexpand(GTK_WIDGET(g_paste_button), TRUE);
    g_signal_connect(g_paste_button, "clicked", G_CALLBACK(begin_capture), "paste");
    gtk_box_append(GTK_BOX(paste_box), GTK_WIDGET(g_paste_button));

    GtkWidget *paste_clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(paste_clear_btn, "clicked", G_CALLBACK(clear_paste), NULL);
    gtk_box_append(GTK_BOX(paste_box), paste_clear_btn);

    gtk_box_append(GTK_BOX(gen),
        make_note("<small>• <b>Virtual keyboard</b> mode may work incorrectly in such "
                  "applications as Google Chrome, Discord and other Electron/Tauri "
                  "applications. You can try to use <b>Paste via shortcut</b> mode instead "
                  "or <b>Overlay</b> mode as the most stable option.\n"
                  "• <b>Hardware keyboard</b> mode works quite stably everywhere, as it "
                  "emulates physical keyboard presses, but cannot work with multilingual "
                  "input since it directly depends on the current keyboard layout.</small>"));

    /* ================= Other ================= */
    GtkWidget *oth = make_page();

    gtk_box_append(GTK_BOX(oth),
        make_switch_row(&g_autodetect_switch, "autodetect_lang", "Auto-detect keyboard layout"));
    gtk_box_append(GTK_BOX(oth),
        make_note("<small>Works on Hyprland, Sway and X11/Xwayland systems. Other "
                  "compositors keep the selected language.</small>"));

    gtk_box_append(GTK_BOX(oth), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(oth), make_title("Sound"));
    gtk_box_append(GTK_BOX(oth),
        make_switch_row(&g_sound_switch, "sound", "Play sounds on start and stop"));

    gtk_box_append(GTK_BOX(oth),
        make_switch_row(&g_punctuation_switch, "punctuation", "Spoken punctuation (English)"));

    gtk_box_append(GTK_BOX(oth), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(oth), make_title("Global hotkey"));

    GtkWidget *hk_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(oth), hk_box);

    g_hotkey_button = GTK_BUTTON(gtk_button_new_with_label("Not set"));
    gtk_widget_set_hexpand(GTK_WIDGET(g_hotkey_button), TRUE);
    g_signal_connect(g_hotkey_button, "clicked", G_CALLBACK(begin_capture), NULL);
    gtk_box_append(GTK_BOX(hk_box), GTK_WIDGET(g_hotkey_button));

    GtkWidget *hk_clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(hk_clear_btn, "clicked", G_CALLBACK(clear_hotkey), NULL);
    gtk_box_append(GTK_BOX(hk_box), hk_clear_btn);

    gtk_box_append(GTK_BOX(oth),
        make_note("<small>Toggles start/stop globally, in any session (tty, X11, Wayland).\n"
                  "Clear it to use your compositor's own hotkey instead.\n"
                  "Requires access to /dev/uinput (user in the 'input' group).</small>"));

    gtk_box_append(GTK_BOX(oth), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(oth), make_title("Silence timeout"));

    GtkWidget *timeout_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(oth), timeout_box);
    gtk_box_append(GTK_BOX(timeout_box), gtk_label_new("Stop after silence (seconds):"));
    g_timeout_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 300, 1));
    g_signal_connect(g_timeout_spin, "notify::value", G_CALLBACK(on_timeout_changed), NULL);
    gtk_box_append(GTK_BOX(timeout_box), GTK_WIDGET(g_timeout_spin));

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gen,
        gtk_label_new("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), oth,
        gtk_label_new("Other"));

    /* Status (errors only; plain success shows nothing) */
    g_status_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(g_status_label), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(g_status_label), 20);
    gtk_widget_set_margin_end(GTK_WIDGET(g_status_label), 20);
    gtk_widget_set_margin_bottom(GTK_WIDGET(g_status_label), 10);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(g_status_label));

    gtk_window_set_child(GTK_WINDOW(win), root);

    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroyed), &g_window);
    g_window = win;

    gtk_window_present(GTK_WINDOW(win));

    load_config();
}

int main(int argc, char **argv) {
    if (argc > 1) {
        int port = atoi(argv[1]);
        if (port > 0 && port < 65536) g_port = port;
    }

    // g_application_run re-parses argv and would treat a numeric arg as a
    // file to open; hand GTK a clean argv of just the program name.
    char *clean_argv[] = { argv[0] };

    GtkApplication *app = gtk_application_new("dev.extra-type.settings", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), 1, clean_argv);
    g_object_unref(app);
    return status;
}
