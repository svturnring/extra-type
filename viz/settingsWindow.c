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
#include <adwaita.h>
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
static AdwComboRow     *g_lang_row = NULL;
static AdwSwitchRow    *g_autodetect_row = NULL;
static AdwSwitchRow    *g_sound_row = NULL;
static AdwSwitchRow    *g_stream_row = NULL;
static AdwSwitchRow    *g_punctuation_row = NULL;
static AdwSpinRow      *g_timeout_row = NULL;
static AdwToastOverlay *g_toast_overlay = NULL;

static char *g_current_lang = NULL;
static gboolean g_loading = FALSE;
static guint g_timeout_src = 0;
static AdwShortcutLabel *g_hotkey_label = NULL;
static AdwShortcutLabel *g_paste_label = NULL;
static gboolean g_capturing = FALSE;
static gboolean g_capture_paste = FALSE; /* TRUE: capturing paste combo, not hotkey */
static char *g_hotkey_wire = NULL;

static char *g_paste_wire = NULL;

static GtkWidget *g_window = NULL; /* single settings window */

/* active shortcut dialog */
static AdwDialog *g_shortcut_dialog = NULL;
static AdwShortcutLabel *g_dialog_preview = NULL;  /* live keycap preview in dialog */

/* insertion mode radio group */
static GtkCheckButton *g_insert_buttons[4] = { NULL, NULL, NULL, NULL };

static const char *evdev_to_keysym_name(int code);
static char *wire_to_accel(const char *wire);
static void update_hotkey_label(void);
static void update_paste_label(void);
static void update_dialog_preview(const char *wire);

/* ------------------------------------------------------------------ */
/* Load config from daemon                                             */
/* ------------------------------------------------------------------ */
static void show_status(const char *msg) {
    if (g_toast_overlay && msg && *msg) {
        AdwToast *toast = adw_toast_new(msg);
        adw_toast_overlay_add_toast(g_toast_overlay, toast);
    }
}

static void load_config(void) {
    char *body = http_get("/config");
    if (!body) {
        show_status("Cannot connect to daemon");
        return;
    }

    GError *err = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, -1, &err)) {
        show_status("Invalid JSON from daemon");
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
                adw_combo_row_set_selected(g_lang_row, i);
                break;
            }
        }
    }

    /* Toggles */
    adw_switch_row_set_active(g_autodetect_row,
        json_get_bool(obj, "autodetect_lang", FALSE));
    adw_switch_row_set_active(g_sound_row,
        json_get_bool(obj, "sound", TRUE));
    if (g_stream_row)
        adw_switch_row_set_active(g_stream_row,
            json_get_bool(obj, "stream", TRUE));
    adw_switch_row_set_active(g_punctuation_row,
        json_get_bool(obj, "punctuation", TRUE));

    /* Timeout */
    adw_spin_row_set_value(g_timeout_row,
        json_get_int(obj, "timeout", 0));

    /* Global hotkey */
    g_free(g_hotkey_wire);
    g_hotkey_wire = NULL;
    char *hk = json_get_string(obj, "hotkey");
    if (hk) {
        g_hotkey_wire = g_strdup(hk);
        g_free(hk);
    }
    update_hotkey_label();

    /* Text insertion */
    char *method = json_get_string(obj, "insert_method");
    if (method) {
        int idx = 0;
        if (strcmp(method, "paste") == 0) idx = 1;
        else if (strcmp(method, "pill") == 0) idx = 2;
        else if (strcmp(method, "dotool") == 0) idx = 3;
        for (int i = 0; i < 4; i++)
            if (g_insert_buttons[i])
                gtk_check_button_set_active(g_insert_buttons[i], i == idx);
        g_free(method);
    }
    g_free(g_paste_wire);
    g_paste_wire = NULL;
    char *paste = json_get_string(obj, "paste");
    if (paste) {
        g_paste_wire = g_strdup(paste);
        g_free(paste);
    }
    update_paste_label();

    g_loading = FALSE;

    g_free(body);
    g_object_unref(parser);
}

/* ------------------------------------------------------------------ */
/* Push config to daemon (immediate save)                              */
/* ------------------------------------------------------------------ */
static void push_config(const char *json) {
    int status = http_post("/config", json);
    if (status != 200) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Save failed (HTTP %d)", status);
        show_status(msg);
    }
}

static void on_lang_changed(AdwComboRow *row, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    if (g_loading) return;
    guint idx = adw_combo_row_get_selected(row);
    const char *lang = (idx < G_N_ELEMENTS(LANG_CODES) - 1) ? LANG_CODES[idx] : "en-US";
    char *json = g_strdup_printf("{\"lang\":\"%s\"}", lang);
    push_config(json);
    g_free(json);
}

static void on_bool_changed(AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    if (g_loading) return;
    const char *key = (const char *)user_data;
    char *json = g_strdup_printf("{\"%s\":%s}", key,
        adw_switch_row_get_active(row) ? "true" : "false");
    push_config(json);
    g_free(json);
}

static void on_insert_toggled(GtkCheckButton *btn, gpointer user_data) {
    (void)btn;
    if (g_loading) return;
    if (!gtk_check_button_get_active(btn)) return; /* only the newly selected one */
    int idx = GPOINTER_TO_INT(user_data);
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
    int timeout = (int)adw_spin_row_get_value(g_timeout_row);
    char *json = g_strdup_printf("{\"timeout\":%d}", timeout);
    push_config(json);
    g_free(json);
    return G_SOURCE_REMOVE;
}

static void on_timeout_changed(AdwSpinRow *row, GParamSpec *pspec, gpointer user_data) {
    (void)row; (void)pspec; (void)user_data;
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

/* Map evdev keycodes to X11 keysym names so stored hotkeys like
 * "ctrl+alt+57" can render as real keycaps ("Space"). Covers the letters,
 * digits, navigation and punctuation keys most hotkeys use. */
static const char *evdev_to_keysym_name(int code) {
    switch (code) {
    case 2: case 3: case 4: case 5: case 6:
    case 7: case 8: case 9: case 10: case 11:
        return &"1234567890"[code - 2]; /* 1..9,0 */
    case 16: return "q"; case 17: return "w"; case 18: return "e";
    case 19: return "r"; case 20: return "t"; case 21: return "y";
    case 22: return "u"; case 23: return "i"; case 24: return "o";
    case 25: return "p"; case 30: return "a"; case 31: return "s";
    case 32: return "d"; case 33: return "f"; case 34: return "g";
    case 35: return "h"; case 36: return "j"; case 37: return "k";
    case 38: return "l"; case 44: return "z"; case 45: return "x";
    case 46: return "c"; case 47: return "v"; case 48: return "b";
    case 49: return "n"; case 50: return "m";
    case 57:  return "space";
    case 28:  return "Return";
    case 14:  return "BackSpace";
    case 15:  return "Tab";
    case 1:   return "Escape";
    case 111: return "Up";
    case 116: return "Down";
    case 113: return "Left";
    case 114: return "Right";
    case 110: return "Home";
    case 119: return "End";
    case 112: return "Prior";
    case 117: return "Next";
    case 61:  return "slash";
    case 60:  return "period";
    case 59:  return "comma";
    case 40:  return "apostrophe";
    default:  return NULL;
    }
}

/* Convert a stored wire combo ("ctrl+alt+57" or "ctrl+shift+v") into the
 * Gtk.accelerator_parse format AdwShortcutLabel expects: "<Ctrl><Alt>space".
 * Modifiers get angle brackets, the final key is a bare keysym name. */
static char *wire_to_accel(const char *wire) {
    if (!wire || !*wire) return NULL;
    GString *s = g_string_new(NULL);
    char *dup = g_strdup(wire);
    char *save = NULL;
    for (char *tok = strtok_r(dup, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        char *end = NULL;
        long code = strtol(tok, &end, 10);
        if (end && *end == '\0' && code > 0) {
            /* evdev keycode: map to a keysym name, fall back to "[code]" */
            const char *kn = evdev_to_keysym_name((int)code);
            if (kn) g_string_append_printf(s, "%s", kn);
            else g_string_append_printf(s, "[%d]", (int)code);
        } else if (strcmp(tok, "ctrl") == 0) g_string_append(s, "<Ctrl>");
        else if (strcmp(tok, "alt") == 0) g_string_append(s, "<Alt>");
        else if (strcmp(tok, "super") == 0) g_string_append(s, "<Super>");
        else if (strcmp(tok, "shift") == 0) g_string_append(s, "<Shift>");
        else {
            /* normalize the key name so gtk_accelerator_parse accepts it:
             * "insert" -> "Insert", "v" -> "v", "space" -> "space".
             * gdk_keyval_from_name() returns GDK_KEY_VoidSymbol (0xffffff)
             * for unknown names, so fall back to trying the capitalized form. */
            guint kv = gdk_keyval_from_name(tok);
            if (kv == GDK_KEY_VoidSymbol && tok[0]) {
                char cap[32];
                snprintf(cap, sizeof(cap), "%c%s", (char)g_ascii_toupper(tok[0]), tok + 1);
                kv = gdk_keyval_from_name(cap);
            }
            const char *norm = (kv && kv != GDK_KEY_VoidSymbol) ? gdk_keyval_name(kv) : NULL;
            g_string_append_printf(s, "%s", norm ? norm : tok);
        }
    }
    g_free(dup);
    return g_string_free(s, FALSE);
}

static void update_hotkey_label(void) {
    if (!g_hotkey_label) return;
    char *accel = wire_to_accel(g_hotkey_wire);
    adw_shortcut_label_set_accelerator(g_hotkey_label, accel ? accel : "");
    g_free(accel);
}

static void update_paste_label(void) {
    if (!g_paste_label) return;
    char *accel = wire_to_accel(g_paste_wire);
    adw_shortcut_label_set_accelerator(g_paste_label, accel ? accel : "");
    g_free(accel);
}

static void end_capture(void) {
    g_capturing = FALSE;
    g_capture_paste = FALSE;
}

/* ---------------------------------------------------------------- */
/* Shortcut capture dialog                                           */
/* ---------------------------------------------------------------- */
static void set_shortcut(const char *wire) {
    if (g_capture_paste) {
        g_free(g_paste_wire);
        g_paste_wire = g_strdup(wire);
        char *json = g_strdup_printf("{\"paste\":\"%s\"}", wire);
        push_config(json);
        g_free(json);
        update_paste_label();
    } else {
        g_free(g_hotkey_wire);
        g_hotkey_wire = g_strdup(wire);
        char *json = g_strdup_printf("{\"hotkey\":\"%s\"}", wire);
        push_config(json);
        g_free(json);
        update_hotkey_label();
    }
    update_dialog_preview(wire);
}

static void reset_shortcut(void) {
    if (g_capture_paste) {
        g_free(g_paste_wire);
        g_paste_wire = g_strdup("ctrl+v");
        update_paste_label();
        push_config("{\"paste\":\"ctrl+v\"}");
    } else {
        g_free(g_hotkey_wire);
        g_hotkey_wire = NULL;
        update_hotkey_label();
        push_config("{\"hotkey\":null}");
    }
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
        if (g_shortcut_dialog) {
            AdwDialog *dlg = g_shortcut_dialog;
            g_shortcut_dialog = NULL;
            g_dialog_preview = NULL;
            adw_dialog_force_close(dlg);
        }
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

    set_shortcut(wire);
    g_free(wire);

    end_capture();
    return GDK_EVENT_STOP;
}

/* presets for the paste shortcut dialog */
static const char *PASTE_PRESETS[] = { "ctrl+v", "ctrl+shift+v", "shift+insert", NULL };

static void on_preset_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *wire = (const char *)user_data;
    if (!g_capture_paste) return;
    g_free(g_paste_wire);
    g_paste_wire = g_strdup(wire);
    char *json = g_strdup_printf("{\"paste\":\"%s\"}", wire);
    push_config(json);
    g_free(json);
    update_paste_label();
}

static void update_dialog_preview(const char *wire) {
    if (!g_dialog_preview) return;
    char *accel = wire_to_accel(wire);
    adw_shortcut_label_set_accelerator(g_dialog_preview, accel ? accel : "");
    g_free(accel);
}

static void dialog_reset_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    reset_shortcut();
    update_dialog_preview(g_capture_paste ? (g_paste_wire ? g_paste_wire : "ctrl+v")
                                          : g_hotkey_wire);
}

static void dialog_response(AdwAlertDialog *dialog, const char *response,
                            gpointer user_data) {
    (void)dialog; (void)response; (void)user_data;
    g_shortcut_dialog = NULL;
    g_dialog_preview = NULL;
    end_capture();
}

static void on_shortcut_row_activated(AdwActionRow *row, gpointer user_data) {
    (void)row;
    if (g_shortcut_dialog) return; /* already open */
    const char *target = (const char *)user_data;
    g_capturing = TRUE;
    g_capture_paste = (target && strcmp(target, "paste") == 0);

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        g_capture_paste ? "Paste shortcut" : "Global hotkey",
        "Press the key combination to record it. Esc closes the dialog."));
    g_shortcut_dialog = ADW_DIALOG(dialog);
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 400);
    adw_dialog_set_can_close(ADW_DIALOG(dialog), TRUE);

    adw_alert_dialog_add_response(dialog, "done", "Done");
    adw_alert_dialog_set_response_appearance(dialog, "done", ADW_RESPONSE_SUGGESTED);
    g_signal_connect(dialog, "response", G_CALLBACK(dialog_response), NULL);

    /* capture keys while the dialog is open */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_capture_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(dialog), key_ctrl);

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    /* live preview of the current combo */
    AdwPreferencesGroup *cap_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(cap_group, "Capture");
    AdwActionRow *cap_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cap_row), "Listening for keys");
    g_dialog_preview = ADW_SHORTCUT_LABEL(adw_shortcut_label_new(""));
    gtk_widget_set_valign(GTK_WIDGET(g_dialog_preview), GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(cap_row, GTK_WIDGET(g_dialog_preview));
    adw_preferences_group_add(cap_group, GTK_WIDGET(cap_row));
    adw_preferences_page_add(page, cap_group);

    if (g_capture_paste) {
        AdwPreferencesGroup *preset_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(preset_group, "Common shortcuts");
        for (int i = 0; PASTE_PRESETS[i]; i++) {
            char *accel = wire_to_accel(PASTE_PRESETS[i]);
            AdwActionRow *prow = ADW_ACTION_ROW(adw_action_row_new());
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(prow), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(prow), accel ? accel : "");
            AdwShortcutLabel *plabel = ADW_SHORTCUT_LABEL(adw_shortcut_label_new(accel ? accel : ""));
            gtk_widget_set_valign(GTK_WIDGET(plabel), GTK_ALIGN_CENTER);
            adw_action_row_add_suffix(prow, GTK_WIDGET(plabel));
            adw_action_row_set_activatable_widget(prow, GTK_WIDGET(plabel));
            g_signal_connect(prow, "activated", G_CALLBACK(on_preset_clicked),
                (gpointer)PASTE_PRESETS[i]);
            adw_preferences_group_add(preset_group, GTK_WIDGET(prow));
            g_free(accel);
        }
        adw_preferences_page_add(page, preset_group);
    }

    /* reset row */
    AdwPreferencesGroup *reset_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwActionRow *reset_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(reset_row),
        g_capture_paste ? "Reset to default" : "Clear hotkey");
    adw_action_row_set_subtitle(reset_row,
        g_capture_paste ? "Restores Ctrl+V" : "Removes the global hotkey");
    GtkWidget *reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset_btn, "destructive-action");
    gtk_widget_set_valign(reset_btn, GTK_ALIGN_CENTER);
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(dialog_reset_clicked), NULL);
    adw_action_row_add_suffix(reset_row, reset_btn);
    adw_preferences_group_add(reset_group, GTK_WIDGET(reset_row));
    adw_preferences_page_add(page, reset_group);

    /* clamp gives the page proper dialog margins */
    AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());
    adw_clamp_set_maximum_size(clamp, 400);
    adw_clamp_set_child(clamp, GTK_WIDGET(page));
    adw_alert_dialog_set_extra_child(dialog, GTK_WIDGET(clamp));

    adw_dialog_present(ADW_DIALOG(dialog), g_window);

    update_dialog_preview(g_capture_paste ? (g_paste_wire ? g_paste_wire : "ctrl+v")
                                          : g_hotkey_wire);
}

/* ------------------------------------------------------------------ */
/* Activate                                                            */
/* ------------------------------------------------------------------ */
static void row_set_subtitle(GtkWidget *row, const char *subtitle) {
    if (subtitle && ADW_IS_ACTION_ROW(row)) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    }
}

static AdwSwitchRow *make_switch_row(const char *key, const char *title,
                                     const char *subtitle) {
    AdwSwitchRow *row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    row_set_subtitle(GTK_WIDGET(row), subtitle);
    /* key is always a static string literal, safe as user_data */
    g_signal_connect(row, "notify::active", G_CALLBACK(on_bool_changed),
                     (gpointer)key);
    if (strcmp(key, "stream") == 0) g_stream_row = row;
    else if (strcmp(key, "autodetect_lang") == 0) g_autodetect_row = row;
    else if (strcmp(key, "sound") == 0) g_sound_row = row;
    else if (strcmp(key, "punctuation") == 0) g_punctuation_row = row;
    return row;
}

static AdwActionRow *make_shortcut_row(AdwShortcutLabel **out_label,
                                       const char *title, const char *subtitle,
                                       const char *capture_target) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    row_set_subtitle(GTK_WIDGET(row), subtitle);

    /* keycap rendering of the combo */
    AdwShortcutLabel *label = ADW_SHORTCUT_LABEL(adw_shortcut_label_new(""));
    adw_shortcut_label_set_disabled_text(label, "Not set");
    gtk_widget_set_valign(GTK_WIDGET(label), GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(row, GTK_WIDGET(label));
    *out_label = label;

    /* clicking the row opens the capture dialog */
    adw_action_row_set_activatable_widget(row, GTK_WIDGET(label));
    g_signal_connect(row, "activated", G_CALLBACK(on_shortcut_row_activated),
                     (gpointer)capture_target);

    return row;
}

static void on_window_destroyed(GtkWidget *widget, gpointer data) {
    (void)widget;
    GtkWidget **win = (GtkWidget **)data;
    *win = NULL;
}

static void activate(AdwApplication *app, gpointer user_data) {
    (void)user_data;

    /* single-instance: a second activation must just raise the existing
     * window instead of creating another one */
    if (g_window) {
        gtk_window_present(GTK_WINDOW(g_window));
        return;
    }

    AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(GTK_WINDOW(win), "Extra Type - Settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 540, 640);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE); /* fixed width/height */

    /* toast overlay wraps the whole content; errors surface as toasts too */
    AdwToastOverlay *toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    g_toast_overlay = toast_overlay;

    /* root: view switcher on top, always-visible status line below */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
    gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(stack));

    /* ================= General ================= */
    AdwPreferencesPage *gen = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup *lang_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(lang_group, "Language");

    GtkStringList *lang_list = gtk_string_list_new(NULL);
    for (int i = 0; LANG_LABELS[i]; i++) {
        char label[64];
        snprintf(label, sizeof(label), "%s  (%s)", LANG_LABELS[i], LANG_CODES[i]);
        gtk_string_list_append(lang_list, label);
    }
    g_lang_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_lang_row), "Recognition language");
    adw_combo_row_set_model(g_lang_row, G_LIST_MODEL(lang_list));
    g_signal_connect(g_lang_row, "notify::selected", G_CALLBACK(on_lang_changed), NULL);
    adw_preferences_group_add(lang_group, GTK_WIDGET(g_lang_row));

    AdwPreferencesGroup *stream_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(stream_group, "Stream");
    adw_preferences_group_add(stream_group,
        GTK_WIDGET(make_switch_row("stream", "Live typing while dictating",
            "Correct only in native Wayland apps; in Chrome and Electron apps "
            "(Discord) live typing can garble or erase text. For reliable input "
            "everywhere, turn Live typing off and use Paste via clipboard, or use "
            "the Overlay mode instead.")));

    AdwPreferencesGroup *insert_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(insert_group, "Insertion mode");

    const char *insert_titles[4] = {
        "Virtual keyboard",
        "Paste via shortcut",
        "Overlay",
        "Hardware keyboard",
    };
    const char *insert_subs[4] = {
        "Types characters as exact keysyms through a virtual Wayland keyboard. "
        "Works in any app accepting keystrokes; layout-independent for native-Wayland apps.",
        "Copies the text to the clipboard and presses the paste shortcut. "
        "Works everywhere including XWayland apps.",
        "Nothing is typed; the transcript collects into the on-screen overlay "
        "and is copied out with its buttons. Works in every app.",
        "Types through /dev/uinput as a real physical keyboard. English only; "
        "the target app must use the US layout.",
    };
    GtkCheckButton *group_btn = NULL;
    for (int i = 0; i < 4; i++) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), insert_titles[i]);
        adw_action_row_set_subtitle(row, insert_subs[i]);
        GtkCheckButton *rb = GTK_CHECK_BUTTON(gtk_check_button_new());
        gtk_check_button_set_group(rb, group_btn);
        gtk_widget_set_valign(GTK_WIDGET(rb), GTK_ALIGN_CENTER);
        gtk_widget_set_halign(GTK_WIDGET(rb), GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(row, GTK_WIDGET(rb));
        adw_action_row_set_activatable_widget(row, GTK_WIDGET(rb));
        g_signal_connect(rb, "toggled", G_CALLBACK(on_insert_toggled), GINT_TO_POINTER(i));
        group_btn = rb;
        g_insert_buttons[i] = rb;
        adw_preferences_group_add(insert_group, GTK_WIDGET(row));
    }

    adw_preferences_page_add(gen, lang_group);
    adw_preferences_page_add(gen, stream_group);
    adw_preferences_page_add(gen, insert_group);

    AdwPreferencesGroup *paste_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(paste_group, "Paste shortcut");
    adw_preferences_group_add(paste_group,
        GTK_WIDGET(make_shortcut_row(&g_paste_label, "Combo",
            "Combo used by the Paste mode", "paste")));
    adw_preferences_page_add(gen, paste_group);

    /* ================= Other ================= */
    AdwPreferencesPage *oth = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup *auto_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(auto_group, "Detection");
    adw_preferences_group_add(auto_group,
        GTK_WIDGET(make_switch_row("autodetect_lang", "Auto-detect keyboard layout",
            "Works on Hyprland, Sway and X11/Xwayland systems. Other compositors "
            "keep the selected language.")));

    AdwPreferencesGroup *sound_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(sound_group, "Sound");
    adw_preferences_group_add(sound_group,
        GTK_WIDGET(make_switch_row("sound", "Play sounds on start and stop", NULL)));
    adw_preferences_group_add(sound_group,
        GTK_WIDGET(make_switch_row("punctuation", "Spoken punctuation (English)", NULL)));

    AdwPreferencesGroup *hk_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(hk_group, "Global hotkey");
    adw_preferences_group_set_description(hk_group,
        "Toggles start/stop globally, in any session (tty, X11, Wayland). Clear it "
        "to use your compositor's own hotkey instead. Requires access to "
        "/dev/uinput (user in the 'input' group).");
    adw_preferences_group_add(hk_group,
        GTK_WIDGET(make_shortcut_row(&g_hotkey_label, "Combo", NULL, NULL)));

    AdwPreferencesGroup *timeout_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(timeout_group, "Silence timeout");

    g_timeout_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(0, 300, 1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_timeout_row),
        "Stop after silence (seconds)");
    g_signal_connect(g_timeout_row, "notify::value", G_CALLBACK(on_timeout_changed), NULL);
    adw_preferences_group_add(timeout_group, GTK_WIDGET(g_timeout_row));

    adw_preferences_page_add(oth, auto_group);
    adw_preferences_page_add(oth, sound_group);
    adw_preferences_page_add(oth, hk_group);
    adw_preferences_page_add(oth, timeout_group);

    adw_view_stack_add_titled(stack, GTK_WIDGET(gen), "general", "General");
    adw_view_stack_add_titled(stack, GTK_WIDGET(oth), "other", "Other");

    /* header bar with the view switcher between the window buttons */
    GtkWidget *header_bar = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header_bar), TRUE);
    AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(switcher, stack);
    adw_view_switcher_set_policy(switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header_bar), GTK_WIDGET(switcher));

    adw_toast_overlay_set_child(toast_overlay, root);

    AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar, header_bar);
    adw_toolbar_view_set_top_bar_style(toolbar, ADW_TOOLBAR_FLAT);
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(toast_overlay));
    adw_application_window_set_content(win, GTK_WIDGET(toolbar));

    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroyed), &g_window);
    g_window = GTK_WIDGET(win);

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

    AdwApplication *app = adw_application_new("dev.extra-type.settings", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), 1, clean_argv);
    g_object_unref(app);
    return status;
}
