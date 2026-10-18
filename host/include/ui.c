#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inputblock.h"
#include "instance.h"
#include "respath.h"
#include "tray.h"
#include "watchdog.h"

#include "clay.h"
#include "clay_raylib_renderer.h"
#include "raylib.h"

/* --------------------------------------------------------------- theme --- */

#define COL(r, g, b, a) ((Clay_Color){ (float)(r), (float)(g), (float)(b), (float)(a) })

static const Clay_Color C_BG        = COL(0x0f, 0x0f, 0x0f, 255);
static const Clay_Color C_CARD      = COL(0x20, 0x20, 0x20, 255);
static const Clay_Color C_ROW       = COL(0x18, 0x18, 0x18, 255);
static const Clay_Color C_BORDER    = COL(0x00, 0x00, 0x00, 128);
static const Clay_Color C_FG        = COL(0xe7, 0xe9, 0xee, 255);
static const Clay_Color C_MUTED     = COL(0x9a, 0xa1, 0xb1, 255);
static const Clay_Color C_ACCENT    = COL(0x5b, 0x8c, 0xff, 255);
static const Clay_Color C_ACCENT_HI = COL(0x7a, 0xa4, 0xff, 255);
static const Clay_Color C_LINE      = COL(0x4f, 0x4f, 0x4f, 255);
static const Clay_Color C_OK        = COL(0x4e, 0xcb, 0x84, 255);
static const Clay_Color C_OK_HI     = COL(0x66, 0xe0, 0x9c, 255);
static const Clay_Color C_WARN      = COL(0xe8, 0xa3, 0x4a, 255);
static const Clay_Color C_DANGER    = COL(0xd6, 0x3a, 0x45, 255);
static const Clay_Color C_DANGER_HI = COL(0xf0, 0x50, 0x5b, 255);
static const Clay_Color C_RX        = COL(0xc7, 0x9b, 0xff, 255);
static const Clay_Color C_TRANSPARENT = COL(0, 0, 0, 0);

enum { FONT_BODY = 0, FONT_MONO = 1, FONT_COUNT = 2 };
static Font s_fonts[FONT_COUNT];

#define FS_SMALL    18
#define FS_BODY     20
#define FS_CAPTION  16
#define FS_TITLE    26
#define FS_BANNER   30
#define FS_ATLAS    40

enum { TAB_DEVICES = 0, TAB_CARDS = 1, TAB_COUNT = 2 };
static const char *const s_tab_names[TAB_COUNT] = { "Devices", "Cards" };
static int s_tab = TAB_DEVICES;

static int s_target_fps = UI_FALLBACK_FPS;

/* Toast. */
#define TOAST_SECONDS 2.5
static unsigned long s_toast_seen;
static double        s_toast_until;
static char          s_toast_text[APP_LOG_TEXT_MAX];

/* --------------------------------------------------------------- helpers -- */

static Clay_String dyn(const char *text)
{
    Clay_String s;
    s.isStaticallyAllocated = false;
    s.length = (int32_t)strlen(text);
    s.chars = text;
    return s;
}

static void HandleClayErrors(Clay_ErrorData e)
{
    fprintf(stderr, "clay: %.*s\n", (int)e.errorText.length, e.errorText.chars);
}

static Clay_Color mix(Clay_Color base, Clay_Color hi, bool on)
{
    return on ? hi : base;
}

static bool clicked(bool enabled)
{
    return enabled && Clay_Hovered() && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* Per-frame scratch for text built from numbers or truncated. Sized for every
   log line (up to the ring's capacity) plus the device rows at once, since a
   frame lays them all out. dyn() does not use these -- it wraps the caller's
   own buffer. s_text_slot resets to 0 each frame. */
#define TEXT_SLOTS   512
#define TEXT_SLOT_MAX 128
static char s_text[TEXT_SLOTS][TEXT_SLOT_MAX];
static int  s_text_slot;

static char *text_slot(void)
{
    if (s_text_slot >= TEXT_SLOTS) {
        s_text_slot = 0;
    }
    return s_text[s_text_slot++];
}

static const char *fmt(const char *f, ...)
{
    char *buf = text_slot();
    va_list a;
    va_start(a, f);
    vsnprintf(buf, TEXT_SLOT_MAX, f, a);
    va_end(a);
    return buf;
}

/*
 * Hard cap on a string's length so a long, unwrappable run (a device name, a
 * JSON log line) can never push a container past the window. Text with spaces
 * still word-wraps; this is the backstop for text without any. Cuts on a UTF-8
 * boundary so an accented name never ends mid-character.
 */
static const char *ellipsize(const char *s, int maxchars)
{
    if ((int)strlen(s) <= maxchars) {
        return s;               /* no copy: dyn() can wrap the original */
    }
    int n = maxchars < TEXT_SLOT_MAX - 4 ? maxchars : TEXT_SLOT_MAX - 4;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;                    /* do not split a multi-byte character */
    }
    char *buf = text_slot();
    memcpy(buf, s, (size_t)n);
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = '\0';
    return buf;
}

/* ------------------------------------------------------------- widgets --- */

static bool ui_button_c(Clay_ElementId id, const char *label, Clay_Color fill,
                        Clay_Color fill_hi, bool enabled)
{
    bool hit = false;
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(38) },
            .padding = { 16, 16, 8, 8 },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = !enabled ? C_LINE : mix(fill, fill_hi, Clay_Hovered()),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
    }) {
        hit = clicked(enabled);
        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FS_BODY,
            .textColor = enabled ? C_FG : C_MUTED }));
    }
    return hit;
}

static bool ui_button(Clay_ElementId id, const char *label, bool primary, bool enabled)
{
    return ui_button_c(id, label, primary ? C_ACCENT : C_CARD,
                       primary ? C_ACCENT_HI : C_LINE, enabled);
}

/* A small square tick box; returns true on the frame it is clicked. */
static bool ui_tick(Clay_ElementId id, bool on)
{
    bool hit = false;
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22) },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = on ? C_ACCENT : C_BG,
        .cornerRadius = CLAY_CORNER_RADIUS(5),
        .border = { .color = on ? C_ACCENT_HI : C_LINE, .width = { 1, 1, 1, 1 } },
    }) {
        hit = clicked(true);
        if (on) {
            CLAY_TEXT(CLAY_STRING("x"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FS_CAPTION, .textColor = C_FG }));
        }
    }
    return hit;
}

#define UI_CARD(id) CLAY(id, {                                               \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },           \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },            \
    })

/* As UI_CARD but fills the remaining height, so a scrolling list inside it is
   bounded and actually scrolls instead of pushing the card past the window. */
#define UI_CARD_FILL(id) CLAY(id, {                                          \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },          \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },            \
    })

static Clay_Color log_colour(app_log_kind_t kind)
{
    switch (kind) {
    case APP_LOG_TX:    return C_ACCENT;
    case APP_LOG_RX:    return C_RX;
    case APP_LOG_EVENT: return C_OK;
    case APP_LOG_ERROR: default: return C_DANGER_HI;
    }
}

/* --------------------------------------------------------------- header -- */

static void ui_header(app_t *app)
{
    CLAY(CLAY_ID("Header"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
    }) {
        CLAY_TEXT(CLAY_STRING("S3 KillSwitch"), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FS_TITLE, .textColor = C_FG }));

        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

        CLAY_AUTO_ID({
            .layout = { .padding = { 12, 12, 6, 6 } },
            .cornerRadius = CLAY_CORNER_RADIUS(999),
            .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
        }) {
            CLAY_TEXT(dyn(app->connected ? app->device_name : "searching..."),
                      CLAY_TEXT_CONFIG({
                          .fontId = FONT_MONO, .fontSize = FS_SMALL,
                          .textColor = app->connected ? C_OK : C_MUTED }));
        }

        if (ui_button(CLAY_ID("Save"), "Save", false, app->config_dirty)) {
            app_config_save(app);
            app_notify(app, "Configuration saved");
        }
        if (ui_button(CLAY_ID("Reload"), "Reload", false, true)) {
            app_config_reload(app);
        }
    }
}

/* -------------------------------------------------------------- banner --- */

static void ui_status_banner(app_t *app)
{
    bool armed = app->armed;

    CLAY(CLAY_ID("Banner"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 16,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = armed ? COL(0x3a, 0x12, 0x14, 255)
                                 : COL(0x12, 0x2a, 0x1c, 255),
        .cornerRadius = CLAY_CORNER_RADIUS(12),
        .border = { .color = armed ? C_DANGER : C_OK, .width = { 1, 1, 1, 1 } },
    }) {
        CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 2 } }) {
            CLAY_TEXT(dyn(armed ? "ENGAGED" : "DISARMED"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FS_BANNER,
                .textColor = armed ? C_DANGER_HI : C_OK_HI }));
            CLAY_TEXT(dyn(armed ? "input from selected keyboards/mice is blocked"
                                : "selected keyboards and mice work normally"),
                      CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_CAPTION,
                                         .textColor = C_MUTED }));
        }

        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

        bool can = (inputblock_available() || hid_available()) &&
                   app->cfg.device_count > 0;
        if (armed) {
            if (ui_button_c(CLAY_ID("Toggle"), "Restore input",
                            C_OK, C_OK_HI, can)) {
                app_toggle(app);
            }
        } else {
            if (ui_button_c(CLAY_ID("Toggle"), "Block input",
                            C_DANGER, C_DANGER_HI, can)) {
                app_toggle(app);
            }
        }
    }

    if (!hid_available()) {
        CLAY_AUTO_ID({ .layout = { .padding = { 2, 2, 0, 0 } } }) {
            CLAY_TEXT(dyn(hid_last_error()), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FS_CAPTION, .textColor = C_WARN }));
        }
    }
}

/* --------------------------------------------------------------- tabs ---- */

static void ui_tab_bar(void)
{
    CLAY(CLAY_ID("TabBar"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(42) },
                    .childGap = 4 },
    }) {
        for (int i = 0; i < TAB_COUNT; i++) {
            bool active = (s_tab == i);
            CLAY(CLAY_IDI("Tab", i), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = (active || Clay_Hovered()) ? C_CARD : C_TRANSPARENT,
                .cornerRadius = (Clay_CornerRadius){ 8, 8, 0, 0 },
            }) {
                if (clicked(!active)) {
                    s_tab = i;
                }
                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0) },
                                .padding = { 20, 20, 0, 0 },
                                .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                }) {
                    CLAY_TEXT(dyn(s_tab_names[i]), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = FS_BODY,
                        .textColor = active ? C_FG : C_MUTED }));
                }
                CLAY_AUTO_ID({
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(2) } },
                    .backgroundColor = active ? C_ACCENT : C_TRANSPARENT,
                }) {}
            }
        }
    }
}

/* ----------------------------------------------------------- devices ---- */

static void ui_devices_tab(app_t *app)
{
    UI_CARD_FILL(CLAY_ID("DevicesCard")) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            /* GROW wrapper so the line wraps to the card rather than overflowing. */
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {
                CLAY_TEXT(dyn(fmt("%d device(s) - tick the ones to control", app->hid_count)),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_CAPTION,
                                             .textColor = C_MUTED }));
            }
            if (ui_button(CLAY_ID("Rescan"), "Rescan", false, true)) {
                app_refresh_hids(app);
            }
        }

        CLAY(CLAY_ID("DeviceList"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .padding = { 0, 12, 0, 0 },
                .childGap = 6,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            if (app->hid_count == 0) {
                CLAY_TEXT(CLAY_STRING("No keyboards or mice found."),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_BODY,
                                             .textColor = C_MUTED }));
            }

            for (int i = 0; i < app->hid_count; i++) {
                const hid_device_t *d = &app->hids[i];
                bool selected = app_device_selected(app, d->instance_id);

                CLAY(CLAY_IDI("Dev", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .padding = CLAY_PADDING_ALL(10),
                        .childGap = 12,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = C_ROW,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                }) {
                    if (ui_tick(CLAY_IDI("DevTick", i), selected)) {
                        if (selected) {
                            config_remove_device(&app->cfg, d->instance_id);
                        } else {
                            config_add_device(&app->cfg, d->instance_id, d->name,
                                              d->vid, d->pid);
                        }
                        app->config_dirty = true;
                    }

                    CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                               .childGap = 2,
                                               .sizing = { CLAY_SIZING_GROW(0) } } }) {
                        CLAY_TEXT(dyn(ellipsize(d->name, 46)), CLAY_TEXT_CONFIG({
                            .fontId = FONT_BODY, .fontSize = FS_BODY, .textColor = C_FG }));
                        /* kind + identity + whether Windows allows disabling,
                           e.g. "keyboard + mouse  046D:C52B  - cannot disable" */
                        const char *kind = d->has_keyboard && d->has_mouse ? "keyboard + mouse"
                                         : d->has_keyboard ? "keyboard"
                                         : d->has_mouse ? "mouse" : "input";
                        const char *who = (d->vid || d->pid)
                                        ? fmt("%04X:%04X", d->vid, d->pid)
                                        : "no USB id";
                        const char *idtxt = d->disableable
                                          ? fmt("%s   %s", kind, who)
                                          : fmt("%s   %s   - cannot disable", kind, who);
                        CLAY_TEXT(dyn(idtxt),
                                  CLAY_TEXT_CONFIG({ .fontId = FONT_MONO, .fontSize = FS_SMALL,
                                                     .textColor = d->disableable ? C_MUTED : C_WARN }));
                    }

                    CLAY_AUTO_ID({
                        .layout = { .padding = { 10, 10, 4, 4 } },
                        .cornerRadius = CLAY_CORNER_RADIUS(999),
                        .backgroundColor = d->disabled ? COL(0x3a, 0x12, 0x14, 255)
                                                       : COL(0x12, 0x2a, 0x1c, 255),
                    }) {
                        CLAY_TEXT(dyn(d->disabled ? "disabled" : "enabled"),
                                  CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_CAPTION,
                                                     .textColor = d->disabled ? C_DANGER_HI : C_OK_HI }));
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------- cards ----- */

static void ui_cards_tab(app_t *app)
{
    UI_CARD(CLAY_ID("ScanCard")) {
        CLAY_TEXT(CLAY_STRING("Last scan"), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = FS_CAPTION, .textColor = C_MUTED }));

        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 12,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (!app->have_scan) {
                CLAY_TEXT(CLAY_STRING("waiting for a card..."), CLAY_TEXT_CONFIG({
                    .fontId = FONT_MONO, .fontSize = FS_BODY, .textColor = C_MUTED }));
            } else {
                CLAY_TEXT(dyn(app->last_uid), CLAY_TEXT_CONFIG({
                    .fontId = FONT_MONO, .fontSize = FS_TITLE, .textColor = C_FG }));
                CLAY_TEXT(dyn(app->last_valid ? "authorised" : "not recognised"),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_CAPTION,
                                             .textColor = app->last_valid ? C_OK_HI : C_WARN }));
            }

            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

            bool addable = app->have_scan && !config_has_card(&app->cfg, app->last_uid);
            if (ui_button(CLAY_ID("AddCard"), "Authorise this card", true, addable)) {
                if (config_add_card(&app->cfg, app->last_uid)) {
                    app->config_dirty = true;
                    app_notify(app, "Card authorised");
                }
            }
        }
    }

    UI_CARD_FILL(CLAY_ID("CardsCard")) {
        CLAY_TEXT(dyn(fmt("Authorised cards (%d)", app->cfg.card_count)),
                  CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_CAPTION,
                                     .textColor = C_MUTED }));

        CLAY(CLAY_ID("CardList"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .padding = { 0, 12, 0, 0 },
                        .childGap = 6, .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            if (app->cfg.card_count == 0) {
                CLAY_TEXT(CLAY_STRING("No cards yet. Scan one, then authorise it above."),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = FS_BODY,
                                             .textColor = C_MUTED }));
            }
            for (int i = 0; i < app->cfg.card_count; i++) {
                CLAY(CLAY_IDI("Card", i), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                .padding = CLAY_PADDING_ALL(10), .childGap = 12,
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = C_ROW,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                }) {
                    CLAY_TEXT(dyn(app->cfg.cards[i].uid), CLAY_TEXT_CONFIG({
                        .fontId = FONT_MONO, .fontSize = FS_BODY, .textColor = C_FG }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}
                    if (ui_button_c(CLAY_IDI("CardDel", i), "Remove",
                                    C_CARD, C_DANGER, true)) {
                        /* snapshot: removing shifts the list under us */
                        char uid[CONFIG_UID_MAX];
                        snprintf(uid, sizeof(uid), "%s", app->cfg.cards[i].uid);
                        config_remove_card(&app->cfg, uid);
                        app->config_dirty = true;
                    }
                }
            }
        }
    }
}

/* --------------------------------------------------------------- log ----- */

/* Copy the whole log to the clipboard as one block of text. */
static void ui_log_copy(app_t *app)
{
    size_t cap = (size_t)app->log_count * (APP_LOG_TEXT_MAX + 1) + 1;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return;
    }
    size_t n = 0;
    for (int i = 0; i < app->log_count && n < cap; i++) {
        const app_log_entry_t *e = app_log_at(app, i);
        if (e == NULL) {
            continue;
        }
        int w = snprintf(buf + n, cap - n, "%s\n", e->text);
        if (w < 0) {
            break;
        }
        n += (size_t)w;
    }
    SetClipboardText(buf);
    free(buf);
    app_notify(app, "Log copied to clipboard");
}

/*
 * Soft-wrap: copy src into dst inserting a newline every @p width display
 * characters, counted as UTF-8 code points and never splitting one. Clay word
 * wrap only breaks on spaces and newlines, so this is what lets a long
 * space-free run (a JSON log line) wrap instead of overflowing.
 */
static void wrap_text(char *dst, size_t cap, const char *src, int width)
{
    size_t o = 0;
    int col = 0;
    for (size_t i = 0; src[i] && o + 2 < cap; ) {
        unsigned char c = (unsigned char)src[i];
        int clen = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;

        if (c == '\n') {
            col = 0;
            dst[o++] = src[i++];
            continue;
        }
        if (col >= width && o + 1 < cap) {
            dst[o++] = '\n';
            col = 0;
        }
        for (int k = 0; k < clen && src[i] && o + 1 < cap; k++) {
            dst[o++] = src[i++];
        }
        col++;
    }
    dst[o] = '\0';
}

/* One wrapped copy per log entry, stable for the whole frame's layout+render. */
static char s_logwrap[APP_LOG_CAPACITY][APP_LOG_TEXT_MAX + 16];

static void ui_log(app_t *app)
{
    UI_CARD(CLAY_ID("LogCard")) {
        CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 8,
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(CLAY_STRING("Traffic"), CLAY_TEXT_CONFIG({
                .fontId = FONT_BODY, .fontSize = FS_CAPTION, .textColor = C_MUTED }));
            CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}
            if (ui_button(CLAY_ID("LogCopy"), "Copy", false, app->log_count > 0)) {
                ui_log_copy(app);
            }
            if (ui_button(CLAY_ID("LogClear"), "Clear", false, app->log_count > 0)) {
                app_log_clear(app);
            }
        }

        CLAY(CLAY_ID("LogScroll"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(120) }, .padding = { 0, 12, 0, 0 },
                        .childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        }) {
            /* Wrap to whatever actually fits the current window width, so a
               resized (narrower) window never overflows. */
            float charw = MeasureTextEx(s_fonts[FONT_MONO], "0", (float)FS_SMALL, 0.0f).x;
            int wrapchars = 56;
            if (charw > 1.0f) {
                int c = (int)((float)(GetScreenWidth() - 96) / charw);
                if (c >= 8) {
                    wrapchars = c;
                }
            }

            for (int i = 0; i < app->log_count; i++) {
                const app_log_entry_t *e = app_log_at(app, i);
                if (e == NULL) {
                    continue;
                }
                wrap_text(s_logwrap[i], sizeof(s_logwrap[i]), e->text, wrapchars);
                CLAY(CLAY_IDI("Log", i), { .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {
                    CLAY_TEXT(dyn(s_logwrap[i]), CLAY_TEXT_CONFIG({
                        .fontId = FONT_MONO, .fontSize = FS_SMALL,
                        .textColor = log_colour(e->kind) }));
                }
            }
        }
    }
}

/* ---------------------------------------------------------- scrollbar ---- */

/*
 * A draggable vertical scrollbar for a clip/scroll container, drawn with raylib
 * over the panel after Clay has rendered. Clay still handles the wheel; this
 * adds the visible, grabbable bar. One at a time is interactive, which is all
 * that is ever hovered.
 */
static void ui_scrollbar(Clay_ElementId id)
{
    Clay_ElementData el = Clay_GetElementData(id);
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    if (!el.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float content = sc.contentDimensions.height;
    float view = sc.scrollContainerDimensions.height;
    if (content <= view + 1.0f) {
        return;                         /* fits: no bar */
    }

    Clay_BoundingBox b = el.boundingBox;
    const float barw = 8.0f;
    const float pad = 2.0f;
    float x = b.x + b.width - barw - pad;
    float track_y = b.y + pad;
    float track_h = b.height - pad * 2.0f;

    float max_scroll = content - view;
    float thumb_h = track_h * (view / content);
    if (thumb_h < 24.0f) {
        thumb_h = 24.0f;
    }

    float scrolled = -sc.scrollPosition->y;     /* Clay offsets negatively */
    if (scrolled < 0) scrolled = 0;
    if (scrolled > max_scroll) scrolled = max_scroll;
    float thumb_y = track_y + (track_h - thumb_h) * (scrolled / max_scroll);

    DrawRectangleRounded((Rectangle){ x, track_y, barw, track_h }, 0.5f, 4,
                         (Color){ 0x2a, 0x2a, 0x2a, 255 });

    static Clay_ElementId s_drag_id;
    static bool  s_dragging;
    static float s_grab_dy;

    Vector2 m = GetMousePosition();
    Rectangle thumb = { x, thumb_y, barw, thumb_h };

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(m, thumb)) {
        s_dragging = true;
        s_drag_id = id;
        s_grab_dy = m.y - thumb_y;
    }
    if (s_dragging && s_drag_id.id == id.id) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            s_dragging = false;
        } else if (track_h > thumb_h) {
            float frac = (m.y - s_grab_dy - track_y) / (track_h - thumb_h);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            sc.scrollPosition->y = -(frac * max_scroll);
            thumb_y = track_y + (track_h - thumb_h) * frac;
        }
    }

    bool hot = CheckCollisionPointRec(m, thumb) || (s_dragging && s_drag_id.id == id.id);
    DrawRectangleRounded((Rectangle){ x, thumb_y, barw, thumb_h }, 0.5f, 4,
                         hot ? (Color){ 0x7a, 0xa4, 0xff, 255 }
                             : (Color){ 0x5b, 0x8c, 0xff, 255 });
}

/* -------------------------------------------------------------- toast ---- */

static void ui_draw_toast(void)
{
    if (GetTime() >= s_toast_until || s_toast_text[0] == '\0') {
        return;
    }
    Font font = s_fonts[FONT_BODY];
    float size = (float)FS_BODY;
    Vector2 m = MeasureTextEx(font, s_toast_text, size, 0.0f);

    float pad = 14.0f;
    float w = m.x + pad * 2;
    float h = m.y + pad * 1.2f;
    float x = (GetScreenWidth() - w) / 2.0f;
    float y = GetScreenHeight() - h - 24.0f;

    /* Rounded fill, square outline: DrawRectangleRoundedLinesEx is raylib 5.5+
       and the vcpkg build is older, so the outline uses the stable call. */
    DrawRectangleRounded((Rectangle){ x, y, w, h }, 0.4f, 8, (Color){ 0x20, 0x20, 0x20, 240 });
    DrawRectangleLinesEx((Rectangle){ x, y, w, h }, 1.0f, (Color){ 0x5b, 0x8c, 0xff, 255 });
    DrawTextEx(font, s_toast_text, (Vector2){ x + pad, y + pad * 0.6f }, size, 0.0f,
               (Color){ 0xe7, 0xe9, 0xee, 255 });
}

/* ----------------------------------------------------------------- run --- */

int ui_run(app_t *app)
{
    uint64_t mem_size = Clay_MinMemorySize();
    void *memory = malloc(mem_size);
    if (memory == NULL) {
        fprintf(stderr, "out of memory setting up the interface\n");
        return 1;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, memory);

    /* Open taller when the debug log is on, so the Traffic container has room
       and nothing is squeezed out of an 800x600 frame. */
    int init_h = app->cfg.debug ? UI_WINDOW_HEIGHT_DEBUG : UI_WINDOW_HEIGHT;

    Clay_Initialize(arena,
                    (Clay_Dimensions){ UI_WINDOW_WIDTH, (float)init_h },
                    (Clay_ErrorHandler){ HandleClayErrors, NULL });

#ifndef _WIN32
    setenv("RESOURCE_NAME", "KillSwitch", 1);
#endif

    /* Fixed size unless debug is on (handled in the loop); without debug the
       window is a fixed 800x600 and cannot be dragged larger or maximised.
       FLAG_WINDOW_HIGHDPI: the manifest marks the process per-monitor DPI
       aware, so on a scaled display (125/150%) the client area is larger than
       800x600 logical. Without this flag raylib lays the UI out at the scaled
       pixel size while the window stays 800 wide, so the content runs off the
       right. The flag makes GetScreenWidth() report the logical size the layout
       is built against and scales the render to the framebuffer. */
    Clay_Raylib_Initialize(UI_WINDOW_WIDTH, init_h, "S3 KillSwitch",
                           FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI);
    SetExitKey(KEY_NULL);

    watchdog_start("killswitch-hang.log");

    s_target_fps = GetMonitorRefreshRate(GetCurrentMonitor());
    if (s_target_fps <= 0) {
        s_target_fps = UI_FALLBACK_FPS;
    }
    SetTargetFPS(s_target_fps);

    char icon_path[512];
    if (respath_find("icon.png", icon_path, sizeof(icon_path))) {
        Image icon = LoadImage(icon_path);
        if (icon.data) {
            ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
    }

    char tray_icon[512];
#ifdef _WIN32
    const char *tray_file = "icon.ico";
#else
    const char *tray_file = "icon.png";
#endif
    if (!respath_find(tray_file, tray_icon, sizeof(tray_icon))) {
        tray_icon[0] = '\0';
    }
    tray_init(GetWindowHandle(), tray_icon, "S3 KillSwitch");
    tray_set_close_hides(true);

    /* Bake ASCII plus Latin-1 (U+0020..U+00FF) into the atlas, so accented
     * device names like "Peripherique clavier" render instead of showing '?'.
     * Passing NULL/0 here would bake only 32..126 (ASCII). */
    int codepoints[224];
    for (int i = 0; i < 224; i++) {
        codepoints[i] = 32 + i;
    }
    const int cp_count = 224;

    char font_path[512];
    if (respath_find("Roboto-Regular.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_BODY] = LoadFontEx(font_path, FS_ATLAS, codepoints, cp_count);
    }
    if (respath_find("RobotoMono-Medium.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_MONO] = LoadFontEx(font_path, FS_ATLAS, codepoints, cp_count);
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        if (!s_fonts[i].glyphs) {
            s_fonts[i] = GetFontDefault();
        } else {
            SetTextureFilter(s_fonts[i].texture, TEXTURE_FILTER_BILINEAR);
        }
    }
    Clay_SetMeasureTextFunction(Raylib_MeasureText, s_fonts);
    Clay_Raylib_SetFonts(s_fonts, FONT_COUNT);

    double last_connect_scan = 0.0;
    double last_hid_refresh = GetTime();
    bool minimized_seen = tray_is_minimized();

    bool resizable = false;

    for (;;) {
        watchdog_phase("WindowShouldClose");
        if (WindowShouldClose()) {
            if (!tray_available()) {
                break;      /* no tray to hide to: quit */
            }
            tray_minimize();
        }

        /* Resizable only with the debug log on; otherwise a fixed 800x600.
           In debug the minimum is the full debug frame, so the list can never
           be narrowed below its content or shortened until the log is clipped;
           it can still be grown for more log. */
        if (app->cfg.debug && !resizable) {
            SetWindowState(FLAG_WINDOW_RESIZABLE);
            SetWindowSize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT_DEBUG);
            SetWindowMinSize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT_DEBUG);
            resizable = true;
        } else if (!app->cfg.debug && resizable) {
            ClearWindowState(FLAG_WINDOW_RESIZABLE);
            SetWindowMinSize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT);
            SetWindowSize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT);
            resizable = false;
        }

        watchdog_phase("tray_poll");
        if (tray_poll()) {
            break;          /* "Close" from the tray menu */
        }

        /* Plug the S3 in at any time and it is picked up on the next sweep. */
        if (!app->connected && GetTime() - last_connect_scan >= UI_CONNECT_SCAN_SECONDS) {
            last_connect_scan = GetTime();
            watchdog_phase("app_connect");
            app_connect(app);
        }

        if (instance_show_requested()) {
            tray_restore();
            instance_raise(GetWindowHandle());
        }

        if (tray_available() && !tray_is_minimized() && IsWindowMinimized()) {
            tray_minimize();
        }

        /* Going to the tray commits the configuration. */
        bool minimized_now = tray_is_minimized();
        if (minimized_now && !minimized_seen && app->config_dirty) {
            app_config_save(app);
        }
        minimized_seen = minimized_now;

        /* Refresh the present-device list on a slow timer. */
        if (GetTime() - last_hid_refresh >= UI_HID_REFRESH_SECONDS) {
            last_hid_refresh = GetTime();
            if (hid_available()) {
                app_refresh_hids(app);
            }
        }

        if (tray_is_minimized()) {
            app_poll(app);
            if (app->notice_seq != s_toast_seen) {
                s_toast_seen = app->notice_seq;
                tray_notify("S3 KillSwitch", app->notice);
            }
            PollInputEvents();
            WaitTime(0.05);
            continue;
        }

        app_poll(app);
        app_debug_pump(app);

        if (app->notice_seq != s_toast_seen) {
            s_toast_seen = app->notice_seq;
            snprintf(s_toast_text, sizeof(s_toast_text), "%s", app->notice);
            s_toast_until = GetTime() + TOAST_SECONDS;
        }

        Clay_SetLayoutDimensions((Clay_Dimensions){
            (float)GetScreenWidth(), (float)GetScreenHeight() });
        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y },
                             IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true,
                                    (Clay_Vector2){ 0, GetMouseWheelMove() * 32 },
                                    GetFrameTime());

        s_text_slot = 0;
        watchdog_phase("layout");
        Clay_BeginLayout();

        CLAY(CLAY_ID("Root"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .padding = { 16, 16, 16, 16 },
                .childGap = 12,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = C_BG,
        }) {
            ui_header(app);
            ui_status_banner(app);
            ui_tab_bar();

            /* The active tab fills the space between the bar and the log. */
            CLAY(CLAY_ID("TabBody"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                            .childGap = 12, .layoutDirection = CLAY_TOP_TO_BOTTOM },
            }) {
                if (s_tab == TAB_DEVICES) {
                    ui_devices_tab(app);
                } else {
                    ui_cards_tab(app);
                }
            }

            if (app->cfg.debug) {
                ui_log(app);
            }
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();

        watchdog_phase("render");
        BeginDrawing();
        ClearBackground((Color){ 0x0f, 0x0f, 0x0f, 255 });
        Clay_Raylib_Render(commands);
        if (s_tab == TAB_DEVICES) {
            ui_scrollbar(CLAY_ID("DeviceList"));
        } else {
            ui_scrollbar(CLAY_ID("CardList"));
        }
        if (app->cfg.debug) {
            ui_scrollbar(CLAY_ID("LogScroll"));
        }
        ui_draw_toast();
        watchdog_phase("present");
        EndDrawing();
    }

    watchdog_stop();
    tray_shutdown();
    Clay_Raylib_Close();
    free(memory);
    return 0;
}
