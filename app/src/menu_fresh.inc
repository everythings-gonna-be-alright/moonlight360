/* The Fresh look: Play, Settings and About. Included at the end of menu.c, so it draws and steers with menu.c's
 * statics and helpers (the items, item_help, play_cover, the finder and picker). Text comes in the console font's
 * sizes: 15 (scale 1 at 80%), 19 (scale 1), 37 (scale 2) and 56 (scale 3).
 *
 * Play: the app A streams, big (its cover on the right, its colour lighting the page), the quality line over the
 * button (A on it opens Settings, Quality), the button, and the other recent apps under it; moving onto one previews it.
 * Settings: the sections down the left (LB and RB step through them), the rows in the middle, and the selected
 * row's help on the right. */

static const uint32_t F_SURFACE = 0xFF19191C, F_SELECTED = 0xFF2A2A2E, F_EDGE = 0x8CF4F4F5, F_BORDER = 0xFF2C2C31,
                      F_TEXT = 0xFFF4F4F5, F_TEXT_2 = 0xFFB3B3B9, F_TEXT_3 = 0xFF8B8B93, F_TRACK = 0xFF38383E,
                      F_BUTTON = 0xFFF4F4F5, F_ON_BUTTON = 0xFF121212, F_ERROR = 0xFFFF5C5C,
                      F_ERROR_TEXT = 0xFFFF6B6B, F_GOOD = 0xFF4CC38A;
enum {
    F_LEFT = 80,
    F_HERO_W = 720,
    F_COVER_X = 928, F_COVER_Y = 160, F_COVER_W = 240, F_COVER_H = 320,
    F_TILE_W = 108, F_TILE_H = 144, F_TILE_GAP = 16, F_SHELF_Y = 464,
    F_TOP = 112,
    F_RAIL_X = 64, F_RAIL_W = 216,
    F_LIST_X = 304, F_LIST_W = 480, F_ROW_H = 54,
    F_PANEL_X = 808, F_PANEL_W = 408, F_PANEL_H = 504,
    F_BUTTON_H = 60,
};

/* Where the cursor is on Play: the quality line, the button, or the shelf (on f_shelf_index; its last place is All
 * apps). */
enum { F_ROW_CHIP, F_ROW_BUTTON, F_ROW_SHELF };
static int f_row = F_ROW_BUTTON;
static int f_shelf_index;
/* Settings: whether the cursor is on the sections at the left (it starts there) or in a section's rows; and the
 * section it was in last, where Settings opens again. */
static bool f_in_rail = true;
static int f_last_section = 1;

/* ---- text and shapes ---- */

static void f_small(int32_t x, int32_t y, uint32_t argb, const char *text)
{
    gfx_scale_begin(0.8f, (float)x, (float)y);
    gfx_text(x, y, 1, argb, text);
    gfx_scale_end();
}

static int32_t f_small_width(const char *text)
{
    return (int32_t)((float)gfx_text_width(text, 1) * 0.8f + 0.5f);
}

static void f_small_right(int32_t right, int32_t y, uint32_t argb, const char *text)
{
    f_small(right - f_small_width(text), y, argb, text);
}

/* Wrapped text, line by line, line_h apart; returns how many lines. */
static int f_lines(int32_t x, int32_t y, uint32_t argb, const char *text, int chars, int32_t line_h, bool small)
{
    char wrapped[1024];
    const int n = gfx_wrap(wrapped, sizeof wrapped, text, chars);
    char *line = wrapped;
    for (int i = 0; line; i++) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (small)
            f_small(x, y + i * line_h, argb, line);
        else
            gfx_text(x, y + i * line_h, 1, argb, line);
        line = next;
    }
    return n;
}

/* The selected thing: a lighter surface with a thin light edge. */
static void f_focus(float x, float y, float w, float h, float r)
{
    gfx_rect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, r + 2.0f, F_EDGE);
    gfx_rect(x, y, w, h, r, F_SELECTED);
}

static void f_switch(int32_t right, int32_t cy, bool on)
{
    const float w = 46.0f, h = 26.0f, x = (float)right - w, y = (float)cy - h / 2;
    gfx_rect(x, y, w, h, h / 2, on ? THEME_ACCENT : F_TRACK);
    gfx_circle(on ? x + w - h / 2 : x + h / 2, (float)cy, h / 2 - 3.0f, on ? F_ON_BUTTON : THEME_KNOB_OFF);
}

/* The night sky, quietly: a few dozen faint stars, fixed; on the right unless everywhere (the startup). */
static void f_stars_at(float strength, bool everywhere)
{
    enum { STARS = 46 };
    static float sx[STARS], sy[STARS], sa[STARS], sp[STARS];
    static bool made;
    if (!made) {
        uint32_t seed = 7;
        for (int i = 0; i < STARS; i++) {
            seed = seed * 1103515245u + 12345u;
            sx[i] = 560.0f + (float)((seed >> 8) % 680u);
            seed = seed * 1103515245u + 12345u;
            sy[i] = 90.0f + (float)((seed >> 8) % 510u);
            seed = seed * 1103515245u + 12345u;
            sa[i] = 0.12f + (float)(seed >> 8 & 0xFF) / 255.0f * 0.33f;
            seed = seed * 1103515245u + 12345u;
            sp[i] = (float)(seed >> 8 & 0xFF) / 255.0f;   /* its own speed and twinkle */
        }
        made = true;
    }
    /* They drift slowly left (the nearer, brighter ones faster), coming round again at the right, and twinkle. */
    const float t = (float)gfx_frame() / 60.0f;
    for (int i = 0; i < STARS; i++) {
        const float drift = everywhere ? 0.0f : fmodf(t * (1.5f + 4.0f * sa[i] * (0.5f + sp[i])), 680.0f);
        float x = sx[i] - drift;
        if (x < 560.0f)
            x += 680.0f;
        if (everywhere)
            x = (sx[i] - 560.0f) * 1.85f + 20.0f;
        const float twinkle = 0.55f + 0.45f * sinf(t * (0.8f + 2.2f * sp[i]) + (float)i * 1.7f);
        const float a = sa[i] * strength * twinkle;
        gfx_circle(x, sy[i], i % 4 ? 1.0f : 1.5f, (uint32_t)(a * 255.0f) << 24 | 0xFFFFFF);
        if (everywhere)   /* twice as many, the second set lower */
            gfx_circle(x + 31.0f, sy[i] + 47.0f, 1.0f, (uint32_t)(a * 180.0f) << 24 | 0xFFFFFF);
    }
}

static void f_stars(void)
{
    f_stars_at(1.0f, false);
}

/* An app's cover's average colour (0xRRGGBB), worked out once; 0 while it has none. */
static uint32_t f_cover_tint(int id)
{
    static int last_id = -1;
    static uint32_t last;
    const uint8_t *px = id > 0 ? coverart_get(config->host, id) : NULL;
    if (!px)
        return 0;
    if (id == last_id)
        return last;
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < APPS_COVER_W * APPS_COVER_H; i += 7, n++) {
        r += px[i * 4];
        g += px[i * 4 + 1];
        b += px[i * 4 + 2];
    }
    /* A little brighter than the mean, so a dark cover still lights the page. */
    r = r / n * 5 / 4, g = g / n * 5 / 4, b = b / n * 5 / 4;
    last = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
    last_id = id;
    return last;
}

/* The cover's light: one soft glow behind it, easing to a new app's colour. */
static void f_glow(uint32_t rgb, float strength)
{
    static float r = -1.0f, g, b, a;
    const float tr = (float)(rgb >> 16 & 0xFF), tg = (float)(rgb >> 8 & 0xFF), tb = (float)(rgb & 0xFF);
    const float ta = rgb ? strength : 0.0f;
    if (r < 0.0f)
        r = tr, g = tg, b = tb, a = ta;
    r += (tr - r) * 0.08f, g += (tg - g) * 0.08f, b += (tb - b) * 0.08f, a += (ta - a) * 0.08f;
    if (a < 0.01f)
        return;
    const float cx = F_COVER_X + F_COVER_W / 2.0f, cy = F_COVER_Y + F_COVER_H / 2.0f, rad = 340.0f;
    gfx_shadow(cx - rad, cy - rad, 2.0f * rad, 2.0f * rad, rad, 300.0f,
               (uint32_t)(a * 255.0f) << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
}

/* Texts written for Classic's tabs, with Fresh's places in their place. */
static const char *f_places(const char *text)
{
    static const char *const swaps[][2] = {
        { "(Network tab)", "(Settings, PC)" },
        { "(Video tab)", "(Settings, Quality)" },
        { "(Play tab)", "(Settings, PC)" },
        { "(Other tab)", "(Settings, Advanced)" },
        { "and that Host PC is its address", "and that it's the PC at the top right" },
        { "App, on this tab.", "Y, All apps." },
    };
    static char buffers[2][768];
    static int next;
    if (!text)
        return NULL;
    char *out = buffers[next];
    next ^= 1;
    snprintf(out, sizeof buffers[0], "%s", text);
    for (size_t i = 0; i < sizeof swaps / sizeof swaps[0]; i++) {
        char *at = strstr(out, swaps[i][0]);
        if (!at)
            continue;
        char rest[768];
        snprintf(rest, sizeof rest, "%s", at + strlen(swaps[i][0]));
        snprintf(at, sizeof buffers[0] - (size_t)(at - out), "%s%s", swaps[i][1], rest);
    }
    return out;
}

/* Play's right side: the app's cover, its colour lighting the page; or, if chosen (Look, Play picture), the moon
 * and its night sky, as Classic has it. Before a PC is set, the moon too. */
static void f_cover(int id, const char *name, float x, float y, float w, float h, uint32_t tint);
static void f_picture(bool host, int id, const char *name, bool error)
{
    if (config->play_moon || !host) {
        f_glow(0, 0.0f);
        moon_draw(F_COVER_X + F_COVER_W / 2.0f, F_COVER_Y + F_COVER_H / 2.0f - 10.0f, 90.0f);
        return;
    }
    f_glow(f_cover_tint(id), error ? 0.10f : 0.30f);
    gfx_shadow(F_COVER_X, F_COVER_Y + 30.0f, F_COVER_W, F_COVER_H, 20.0f, 60.0f, 0x8C000000);
    f_cover(id, name, F_COVER_X, F_COVER_Y, F_COVER_W, F_COVER_H, error ? 0xFF555555 : 0xFFFFFFFF);
}

/* ---- the header: the name, Play · Settings · About, and on Play which PC ---- */

static int f_place(int t)
{
    return t == TAB_PLAY ? 0 : t == FRESH_ABOUT ? 2 : 1;
}

static void f_header(int place, bool pc)
{
    const int32_t cy = THEME_HEADER_Y + 22;
    ui_draw_wordmark(THEME_LEFT, cy);

    static const char *const names[3] = { "Play", "Settings", "About" };
    enum { PAD = 18, END = 34 };
    int32_t w = 2 * END;
    for (int i = 0; i < 3; i++)
        w += gfx_text_width(names[i], 1) + 2 * PAD;
    const int32_t x0 = 640 - w / 2;
    gfx_rect((float)x0, (float)cy - 22.0f, (float)w, 44.0f, 22.0f, F_SURFACE);
    f_small(x0 + 12, cy - 8, F_TEXT_3, "LB");
    f_small_right(x0 + w - 12, cy - 8, F_TEXT_3, "RB");
    /* The white pill slides to the place. */
    static float pill_x = -1.0f, pill_w;
    int32_t x = x0 + END;
    for (int i = 0; i < 3; i++) {
        const int32_t iw = gfx_text_width(names[i], 1) + 2 * PAD;
        if (i == place) {
            pill_x = pill_x < 0.0f ? (float)x : approach(pill_x, (float)x);
            pill_w = pill_w <= 0.0f ? (float)iw : approach(pill_w, (float)iw);
        }
        x += iw;
    }
    gfx_rect(pill_x, (float)cy - 18.0f, pill_w, 36.0f, 18.0f, F_BUTTON);
    x = x0 + END;
    for (int i = 0; i < 3; i++) {
        const int32_t iw = gfx_text_width(names[i], 1) + 2 * PAD;
        gfx_text(x + PAD, cy - 11, 1, i == place ? F_ON_BUTTON : F_TEXT_2, names[i]);
        x += iw;
    }

    if (!pc)
        return;
    /* Which PC: its address, and X to change it. */
    char line[64];
    gfx_fit(line, sizeof line, config->host, 22);
    const int32_t tw = f_small_width(line), cw = 14 + tw + 10 + 28 + 6;
    const float bx = (float)(THEME_RIGHT - cw);
    gfx_rect(bx, (float)cy - 18.0f, (float)cw, 36.0f, 18.0f, F_BORDER);
    gfx_rect(bx + 1.0f, (float)cy - 17.0f, (float)cw - 2.0f, 34.0f, 17.0f, THEME_BG);
    f_small((int32_t)bx + 14, cy - 8, F_TEXT_2, line);
    gfx_button((int32_t)bx + 14 + tw + 10, cy, 'X');
}

/* ---- Play ---- */

typedef struct {
    int id;
    const char *name;
} FApp;

/* The recent apps but the one A streams now, newest first. */
static int f_shelf(FApp *out)
{
    int n = 0;
    if (!config->show_recent || !config->host[0])
        return 0;
    for (int i = 0; i < CONFIG_RECENT && config->recent_name[i][0]; i++)
        if (strcmp(config->recent_name[i], config->app) != 0) {
            out[n].id = config->recent_id[i];
            out[n].name = config->recent_name[i];
            n++;
        }
    return n;
}

static MenuAction fresh_update_play(uint16_t p, uint16_t nav, uint32_t pad)
{
    FApp shelf[CONFIG_RECENT];
    const int n = f_shelf(shelf);
    const bool host = config->host[0] != 0;
    if (!host && f_row != F_ROW_BUTTON)
        f_row = F_ROW_BUTTON;
    if (f_shelf_index > n)
        f_shelf_index = n;

    if ((p & INPUT_GAMEPAD_X) && f_row != F_ROW_SHELF) {   /* another PC */
        uisound_play(UISOUND_SELECT);
        open_finder();
        return MENU_NONE;
    }
    if ((p & INPUT_GAMEPAD_Y) && host) {   /* every app on the PC */
        uisound_play(UISOUND_SELECT);
        open_picker();
        return MENU_NONE;
    }
    switch (f_row) {
    case F_ROW_CHIP:
        if (nav & INPUT_GAMEPAD_DPAD_DOWN) {
            f_row = F_ROW_BUTTON;
            uisound_play(UISOUND_MOVE);
        } else if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {   /* its settings */
            uisound_play(UISOUND_SELECT);
            f_row = F_ROW_BUTTON;
            tab = 1;
        } else if (p & INPUT_GAMEPAD_B) {
            f_row = F_ROW_BUTTON;
            uisound_play(UISOUND_BACK);
        }
        return MENU_NONE;
    case F_ROW_SHELF: {
        const int before = f_shelf_index;
        if ((nav & INPUT_GAMEPAD_DPAD_LEFT) && f_shelf_index > 0)
            f_shelf_index--;
        if ((nav & INPUT_GAMEPAD_DPAD_RIGHT) && f_shelf_index < n)
            f_shelf_index++;
        if (f_shelf_index != before)
            uisound_play(UISOUND_MOVE);
        if ((nav & INPUT_GAMEPAD_DPAD_UP) || (p & INPUT_GAMEPAD_B)) {
            f_row = F_ROW_BUTTON;
            uisound_play((p & INPUT_GAMEPAD_B) ? UISOUND_BACK : UISOUND_MOVE);
            return MENU_NONE;
        }
        if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
            uisound_play(UISOUND_SELECT);
            if (f_shelf_index == n) {   /* All apps */
                open_picker();
                return MENU_NONE;
            }
            snprintf(config->app, sizeof config->app, "%s", shelf[f_shelf_index].name);
            config->app_id = shelf[f_shelf_index].id;
            save();
            f_row = F_ROW_BUTTON;
            return MENU_CONNECT;
        }
        if ((p & INPUT_GAMEPAD_X) && f_shelf_index < n) {   /* off the list */
            for (int i = 0; i < CONFIG_RECENT; i++)
                if (!strcmp(config->recent_name[i], shelf[f_shelf_index].name)) {
                    config_remove_recent(config, i);
                    break;
                }
            save();
            uisound_play(UISOUND_BACK);
        }
        return MENU_NONE;
    }
    default:
        if ((nav & INPUT_GAMEPAD_DPAD_UP) && host && !(message[0] && message_is_error(message))) {
            f_row = F_ROW_CHIP;
            uisound_play(UISOUND_MOVE);
        } else if ((nav & INPUT_GAMEPAD_DPAD_DOWN) && host) {
            f_row = F_ROW_SHELF;
            f_shelf_index = 0;
            uisound_play(UISOUND_MOVE);
        } else if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
            uisound_play(UISOUND_SELECT);
            return activate(ITEM_CONNECT, pad);
        }
        return MENU_NONE;
    }
}

/* A cover, or the app's initial on a tile while it has none. */
static void f_cover(int id, const char *name, float x, float y, float w, float h, uint32_t tint)
{
    GfxImage *image = play_cover(id);
    if (image) {
        gfx_image(image, x, y, w, h, tint);
        return;
    }
    gfx_rect(x, y, w, h, w > 200.0f ? 20.0f : 12.0f, F_SURFACE);
    const char initial[2] = { name && name[0] ? name[0] : '?', 0 };
    const uint32_t scale = w > 200.0f ? 4 : 2;
    gfx_text_center((int32_t)(x + w / 2), (int32_t)(y + h / 2) - 10 * (int32_t)scale, scale, F_TEXT_3, initial);
}

/* The quality line: what it streams at. */
static void f_quality_text(char *out, size_t size)
{
    const char *res = config->height == 1080 ? "1080p" : config->height == 720 ? "720p" : config->height == 900 ? "900p"
                    : config->height == 540 ? "540p" : config->height == 480 ? "480p" : "360p";
    snprintf(out, size, "%s  \xC2\xB7  %d fps  \xC2\xB7  %d Mbps  \xC2\xB7  %s", res, config->fps,
             config->bitrate_kbps / 1000, config->codec == CODEC_MPEG2 ? "MPEG-2" : "H.264");
}

/* The main button: white while it's where A goes, dark otherwise. A little raised, like a key: a darker lip under
 * it and a line of light along its top; pressed, it sinks onto the lip and comes back up. */
static void f_button(int32_t x, int32_t y, const char *label, bool on)
{
    enum { LIP = 5 };
    const int32_t w = gfx_text_width(label, 1) + 26 + 34;
    const float launch = launching ? (float)(gfx_frame() - launch_frame) / LAUNCH_FRAMES : -1.0f;
    const float press = launch >= 0.0f ? sinf(3.14159265f * (launch > 1.0f ? 1.0f : launch)) : 0.0f;
    const float sink = (float)LIP * press, fx = (float)x, fy = (float)y + sink, r = F_BUTTON_H / 2.0f;
    if (on) {
        gfx_shadow(fx, (float)y + 14.0f, (float)w, F_BUTTON_H, r, 30.0f, (uint32_t)(0x73 - 0x30 * press) << 24);
        gfx_rect(fx, (float)y + LIP, (float)w, F_BUTTON_H, r, 0xFFA3A3AB);   /* the lip */
        gfx_rect(fx, fy, (float)w, F_BUTTON_H, r, 0xFFFFFFFF);               /* the top edge's light */
        gfx_rect(fx, fy + 1.5f, (float)w, F_BUTTON_H - 1.5f, r - 0.75f, F_BUTTON);
    } else {
        gfx_rect(fx, (float)y + LIP, (float)w, F_BUTTON_H, r, 0xFF101012);
        gfx_rect(fx, fy, (float)w, F_BUTTON_H, r, F_BORDER);
        gfx_rect(fx + 1.0f, fy + 1.0f, (float)w - 2.0f, F_BUTTON_H - 2.0f, r - 1.0f, F_SURFACE);
    }
    gfx_text(x + 26, (int32_t)(fy + 0.5f) + 19, 1, on ? F_ON_BUTTON : F_TEXT_2, label);
}

/* The other recent apps, and All apps at the end; the one the cursor is on lifted and edged, the rest dimmed. */
static void f_draw_shelf(const FApp *shelf, int n, bool focus, int at)
{
    if (!config->host[0])
        return;
    if (n)
        f_small(F_LEFT, F_SHELF_Y - 32, F_TEXT_3, "Recent");
    for (int i = 0; i <= n; i++) {
        const bool on = focus && i == at;
        const float x = F_LEFT + (float)i * (F_TILE_W + F_TILE_GAP), y = F_SHELF_Y - (on ? 8.0f : 0.0f);
        if (on)
            gfx_rect(x - 2.0f, y - 2.0f, F_TILE_W + 4.0f, F_TILE_H + 4.0f, 14.0f, F_EDGE);
        if (i < n) {
            f_cover(shelf[i].id, shelf[i].name, x, y, F_TILE_W, F_TILE_H,
                    on ? 0xFFFFFFFF : focus ? 0xFF666666 : 0xFFCCCCCC);
            continue;
        }
        gfx_rect(x, y, F_TILE_W, F_TILE_H, 12.0f, on ? F_SELECTED : F_BORDER);
        gfx_rect(x + 1.0f, y + 1.0f, F_TILE_W - 2.0f, F_TILE_H - 2.0f, 11.0f, on ? F_SELECTED : F_SURFACE);
        icon_draw(ICON_APP, x + (F_TILE_W - ICON_SIZE) / 2.0f, y + 44.0f, on ? F_TEXT : F_TEXT_2);
        f_small((int32_t)x + (F_TILE_W - f_small_width("All apps")) / 2, (int32_t)y + 86, on ? F_TEXT : F_TEXT_2,
                "All apps");
    }
}

static void f_play(void)
{
    FApp shelf[CONFIG_RECENT];
    const int n = f_shelf(shelf);
    if (f_shelf_index > n)
        f_shelf_index = n;
    const bool host = config->host[0] != 0;
    const bool error = message[0] && message_is_error(message);
    const bool browsing = f_row == F_ROW_SHELF && f_shelf_index < n;
    /* The app on show: the one the cursor is on in the shelf, else the one A streams. */
    const int id = browsing ? shelf[f_shelf_index].id : config->app_id;
    const char *name = browsing ? shelf[f_shelf_index].name : config->app;

    f_stars();
    f_picture(host, id, name, error);

    char line[160];
    int32_t y = error ? 116 : 150;
    /* The line over the title. */
    if (!host) {
        f_small(F_LEFT, y, THEME_ACCENT, "WELCOME");
    } else if (error) {
        char upper[96];
        snprintf(upper, sizeof upper, "%s DIDN'T START", config->app);
        for (char *c = upper; *c; c++)
            *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
        gfx_fit_width(line, sizeof line, upper, 1, (int32_t)(F_HERO_W / 0.8f) - 30);
        gfx_circle(F_LEFT + 7.0f, (float)y + 8.0f, 7.0f, F_ERROR);
        gfx_text_center(F_LEFT + 7, y - 2, 1, THEME_BG, "!");
        f_small(F_LEFT + 22, y, F_ERROR_TEXT, line);
    } else {
        f_small(F_LEFT, y, THEME_ACCENT, browsing ? "PLAYED BEFORE" : "READY TO STREAM");
    }
    y += 22;

    /* The title: the app, or what went wrong. */
    const char *title, *advice;
    friendly_error(message, &title, &advice);
    const char *big = !host ? "Find your PC" : error ? (title ? title : "That didn't work") : name;
    const uint32_t scale = gfx_text_width(big, 3) <= F_HERO_W ? 3 : 2;
    gfx_fit_width(line, sizeof line, big, scale, F_HERO_W);
    gfx_text(F_LEFT, y + (scale == 3 ? 0 : 10), scale, F_TEXT, line);
    y += 60 + 16;

    if (!host) {
        y += f_lines(F_LEFT, y, F_TEXT_2, "Moonlight streams games from a PC running Sunshine or Apollo, on the same "
                                          "network. A looks for it.", 70, 26, false) * 26 + 24;
    } else if (error) {
        if (advice)
            y += f_lines(F_LEFT, y, F_TEXT_2, f_places(advice), 72, 26, false) * 26 + 4;
        char raw[256];
        size_t k = 0;
        for (const char *c = message; *c && k + 1 < sizeof raw; c++)
            raw[k++] = *c == '\n' ? ' ' : *c;
        raw[k] = 0;
        gfx_fit_width(line, sizeof line, raw, 1, (int32_t)(F_HERO_W / 0.8f));
        f_small(F_LEFT, y + 2, F_TEXT_3, line);
        y += 22 + 20;
    } else {
        /* The quality line, which A on it changes. */
        f_quality_text(line, sizeof line);
        const int32_t cw = 16 + ICON_SIZE + 8 + gfx_text_width(line, 1) + 12 + 16 + 14;
        const bool on = f_row == F_ROW_CHIP;
        if (on)
            f_focus(F_LEFT, (float)y, (float)cw, 40.0f, 20.0f);
        else
            gfx_rect(F_LEFT, (float)y, (float)cw, 40.0f, 20.0f, F_SURFACE);
        icon_draw(ICON_BITRATE, F_LEFT + 12.0f, (float)y + 5.0f, on ? F_TEXT : F_TEXT_2);
        gfx_text(F_LEFT + 16 + ICON_SIZE + 4, y + 9, 1, on ? F_TEXT : F_TEXT_2, line);
        f_small(F_LEFT + cw - 30, y + 12, on ? F_TEXT : F_TEXT_3, "\xE2\x96\xBC");   /* U+25BC */
        y += 40 + 30;
    }

    /* The button, and what's under it: the last outcome, or how to reach the stream menu. */
    const char *label = !host ? ARROW_RIGHT "  Find your PC" : error ? "\xE2\x86\xBB  Try again" : ARROW_RIGHT "  Start streaming";
    f_button(F_LEFT, y, label, f_row == F_ROW_BUTTON);
    y += F_BUTTON_H + 18;
    if (message[0] && !error) {
        char one[512];
        size_t k = 0;
        for (const char *c = message; *c && k + 1 < sizeof one; c++)
            one[k++] = *c == '\n' ? ' ' : *c;
        one[k] = 0;
        gfx_fit_width(line, sizeof line, one, 1, (int32_t)(F_HERO_W / 0.8f));
        f_small(F_LEFT, y, F_TEXT_2, line);
    } else if (host && !error && y < F_SHELF_Y - 60) {
        snprintf(line, sizeof line, "While streaming, hold %s for a second to open the stream menu.",
                 menu_combo_name(config->menu_combo));
        f_small(F_LEFT, y, F_TEXT_3, line);
    }

    f_draw_shelf(shelf, n, f_row == F_ROW_SHELF, f_shelf_index);
}

/* ---- moving between Play, Settings and About, and through Settings ---- */

/* What a section holds, while the cursor is on it in the list of sections. */
static const char *const f_section_about[FRESH_TABS] = {
    NULL,
    "How the stream looks and how quickly it reaches the TV: resolution, frame rate, bitrate and codec, and two "
    "switches for latency and decoding.",
    "The PC you stream from, waking it when it sleeps, and reconnecting to the last game when Moonlight opens.",
    "What opens the stream menu while you play, and whether the controller rumbles.",
    "Where the game's sound plays, the menus' clicks, and how the sound travels from the PC.",
    "How the menus look, the Recent row on Play, and the stats over the game.",
    "Network packets and test switches. Best left as they are unless something's wrong.",
    NULL,
};

/* Forget this PC (Settings, PC): A held this long, then the PC's address, its MAC, the app and the recent apps go,
 * and Play welcomes as on the first run. The console's pairing keys stay: they're the console's, for any PC. */
enum { F_FORGET_FRAMES = 60 };
static bool f_forget_holding;
static uint32_t f_forget_from;

static void f_forget_pc(void)
{
    printf("[I] menu: forgetting the PC %s\n", config->host);
    config->host[0] = 0;
    config->host_mac[0] = 0;
    snprintf(config->app, sizeof config->app, "Desktop");
    config->app_id = 0;
    for (int i = 0; i < CONFIG_RECENT; i++) {
        config->recent_id[i] = 0;
        config->recent_name[i][0] = 0;
    }
    save();
    menu_set_message(NULL);
    tab = TAB_PLAY;
    f_row = F_ROW_BUTTON;
    f_in_rail = true;
}

static MenuAction f_update_settings(uint16_t p, uint16_t nav, uint32_t pad)
{
    f_last_section = tab;
    if (f_in_rail) {   /* choosing a section: its rows show as the cursor moves */
        const int before = tab;
        if ((nav & INPUT_GAMEPAD_DPAD_UP) && tab > 1)
            tab--;
        if ((nav & INPUT_GAMEPAD_DPAD_DOWN) && tab < FRESH_ABOUT - 1)
            tab++;
        if (tab != before)
            uisound_play(UISOUND_MOVE);
        f_last_section = tab;
        if ((p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) || (nav & INPUT_GAMEPAD_DPAD_RIGHT)) {
            f_in_rail = false;
            uisound_play(UISOUND_SELECT);
        } else if (p & INPUT_GAMEPAD_B) {
            tab = TAB_PLAY;
            uisound_play(UISOUND_BACK);
        }
        return MENU_NONE;
    }
    const int count = tabs[tab].count, item = selected_item();
    /* Forget this PC: A held for a second; letting go, or moving, stops it. */
    if (f_forget_holding) {
        bool held = false;
        for (int i = 0; update_input && i < INPUT_PADS; i++)
            held |= (update_input->buttons[i] & INPUT_GAMEPAD_A) != 0;
        if (item != ITEM_FORGET || !held) {
            f_forget_holding = false;
        } else if (gfx_frame() - f_forget_from >= F_FORGET_FRAMES) {
            f_forget_holding = false;
            f_forget_pc();
            uisound_play(UISOUND_BACK);
        }
        return MENU_NONE;
    }
    if (item == ITEM_FORGET && (p & INPUT_GAMEPAD_A)) {
        if (config->host[0]) {
            f_forget_holding = true;
            f_forget_from = gfx_frame();
        }
        return MENU_NONE;
    }
    if (p & INPUT_GAMEPAD_B) {   /* back to the sections */
        f_in_rail = true;
        uisound_play(UISOUND_BACK);
        return MENU_NONE;
    }
    const int before = cursor[tab];
    if ((nav & INPUT_GAMEPAD_DPAD_UP) && cursor[tab] > 0)
        cursor[tab]--;
    if ((nav & INPUT_GAMEPAD_DPAD_DOWN) && cursor[tab] < count - 1)
        cursor[tab]++;
    if (cursor[tab] != before)
        uisound_play(UISOUND_MOVE);
    if (nav & (INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_RIGHT)) {
        if ((item == ITEM_HOST || item == ITEM_FORGET) && (nav & INPUT_GAMEPAD_DPAD_LEFT)) {   /* nothing to change */
            f_in_rail = true;
            uisound_play(UISOUND_BACK);
        } else if (item != ITEM_HOST && item != ITEM_FORGET) {
            uisound_play(UISOUND_CHANGE);
            change(item, (nav & INPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1);
        }
    }
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
        uisound_play(is_toggle(item) ? UISOUND_CHANGE : UISOUND_SELECT);
        return activate(item, pad);
    }
    return MENU_NONE;
}

/* LB and RB: Play, Settings (the section last in, on the list of sections) and About, round. In Settings, its own
 * steering. Returns whether it took the input (and *action what to do). */
static bool fresh_update_nav(uint16_t p, uint16_t nav, uint32_t pad, MenuAction *action)
{
    *action = MENU_NONE;
    if (p & (INPUT_GAMEPAD_LEFT_SHOULDER | INPUT_GAMEPAD_RIGHT_SHOULDER)) {
        const int place = (f_place(tab) + ((p & INPUT_GAMEPAD_LEFT_SHOULDER) ? 2 : 1)) % 3;
        if (tab > TAB_PLAY && tab < FRESH_ABOUT)
            f_last_section = tab;
        tab = place == 0 ? TAB_PLAY : place == 2 ? FRESH_ABOUT : f_last_section;
        if (place == 1)
            f_in_rail = true;
        uisound_play(UISOUND_MOVE);
        return true;
    }
    if (tab > TAB_PLAY && tab < FRESH_ABOUT) {
        *action = f_update_settings(p, nav, pad);
        return true;
    }
    return false;
}

/* ---- Settings ---- */

static bool f_toggle_on(int item)
{
    switch (item) {
    case ITEM_FAST_DECODE: return config->fast_decode;
    case ITEM_LOW_LATENCY: return config->low_latency;
    case ITEM_DECODE: return config->decode;
    case ITEM_DRAW: return config->draw;
    case ITEM_LOGGING: return config->logging;
    case ITEM_SOUNDS: return config->ui_sounds;
    case ITEM_AUTO_RESUME: return config->auto_resume;
    case ITEM_RUMBLE: return config->rumble;
    case ITEM_WAKE: return config->wake_on_lan;
    case ITEM_SHOW_RECENT: return config->show_recent;
    default: return false;
    }
}

/* A row's value as text; *warn when it hurts. */
static void f_value_text(int item, char *out, size_t size, bool *warn)
{
    *warn = false;
    switch (item) {
    case ITEM_RESOLUTION: {
        if (resolution_index() < 0)
            snprintf(out, size, "%dx%d", config->width, config->height);
        else
            snprintf(out, size, "%dp", config->height);
        *warn = config->height > 900 && config->fps > 30;
        break;
    }
    case ITEM_FPS: snprintf(out, size, "%d fps", config->fps); break;
    case ITEM_BITRATE: snprintf(out, size, "%d Mbps", config->bitrate_kbps / 1000); break;
    case ITEM_CODEC:
        snprintf(out, size, "%s", config->codec == CODEC_MPEG2 ? "MPEG-2 (beta)" : "H.264");
        *warn = config->codec == CODEC_MPEG2;
        break;
    case ITEM_STATS: {
        static const char *const names[STATS_MODES] = { "Off", "Compact", "Detailed" };
        snprintf(out, size, "%s", names[config->stats_mode]);
        break;
    }
    case ITEM_HUD_CORNER: {
        static const char *const corners[HUD_CORNERS] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        snprintf(out, size, "%s", corners[config->hud_corner]);
        break;
    }
    case ITEM_HUD_SIZE: {
        static const char *const sizes[HUD_SIZES] = { "Small", "Medium", "Large" };
        snprintf(out, size, "%s", sizes[config->hud_size]);
        break;
    }
    case ITEM_MENU_COMBO: snprintf(out, size, "Hold %s", menu_combo_name(config->menu_combo)); break;
    case ITEM_THEME: snprintf(out, size, "%s", theme->name); break;
    case ITEM_LOOK: snprintf(out, size, "%s", config->look == LOOK_CLASSIC ? "Classic" : "Fresh"); break;
    case ITEM_PLAY_PICTURE: snprintf(out, size, "%s", config->play_moon ? "Moon" : "Cover"); break;
    case ITEM_AUDIO_ON_HOST: snprintf(out, size, "%s", config->audio_on_host ? "PC" : "Xbox"); break;
    case ITEM_AUDIO_PACKETS:
        snprintf(out, size, "%d ms", config->audio_packet_ms);
        *warn = config->audio_packet_ms != 20;
        break;
    case ITEM_PACKET_SIZE: snprintf(out, size, "%d bytes", config->packet_size); break;
    case ITEM_SLICES: snprintf(out, size, "%d", config->slices); break;
    case ITEM_VIDEO_TEST: {
        static const char *const modes[3] = { "Full", "No drawing", "No decoding" };
        const int now = !config->decode ? 2 : !config->draw ? 1 : 0;
        snprintf(out, size, "%s", modes[now]);
        *warn = now != 0;
        break;
    }
    case ITEM_HOST: snprintf(out, size, "%s", config->host[0] ? config->host : "Not set"); break;
    default: out[0] = 0; break;
    }
}

/* A small heading over row i of a section, or NULL. */
static const char *f_heading(int t, int i)
{
    if (t == 5 && i == 4)
        return "STATS OVER THE GAME";
    return NULL;
}

/* What the Quality settings come to from the PC to the TV, from this console's measurements: the PC's encoder
 * (about 4 ms a 1080p frame, logs 2026-11-24), the frame's bits over the link (the console's 100 Mbit/s port, or about
 * 25 Mbit/s its Wi-Fi carries), decoding (a lean 1080p60 stream at 30 Mbit/s: 12.8 ms, and 1.25 ms more for every 100
 * kbit a frame, decbench; scaled to the picture's size) and the TV's part (about 20 ms drawn as soon as decoded,
 * 28 on the fixed beat). An estimate: a hardware encoder's stream decodes about twice as slowly. */
typedef struct {
    float pc, net, decode, display, total, budget;
} FLatency;

static FLatency f_latency(void)
{
    FLatency l;
    const float pixels = (float)config->width * (float)config->height / (1920.0f * 1080.0f);
    const float kbit = (float)config->bitrate_kbps / (float)config->fps;   /* a frame */
    l.pc = 1.5f + 2.5f * pixels;
    l.net = 1.0f + kbit / (on_wifi ? 25000.0f : 100000.0f) * 1000.0f;
    l.decode = 6.55f * pixels + 1.25f * kbit / 100.0f;
    if (config->codec == CODEC_MPEG2)
        l.decode *= 0.7f;
    l.display = config->low_latency ? 20.0f : 28.0f;
    l.budget = 1000.0f / (float)config->fps;
    /* Decoding slower than the frames come: they queue, and some are dropped. */
    const float queue = l.decode > l.budget ? l.decode - l.budget : 0.0f;
    l.total = l.pc + l.net + l.decode + l.display + queue;
    return l;
}

static void f_draw_latency(int32_t x, int32_t y)
{
    const FLatency l = f_latency();
    /* 1080p at 60 fps in H.264 decodes in time only from a lean stream: said here too, with the numbers. */
    const bool lean = config->height > 900 && config->fps > 30 && config->codec != CODEC_MPEG2;
    enum { W = F_LIST_W };
    const float h = lean ? 124.0f : 100.0f;
    gfx_rect((float)x, (float)y, W, h, 18.0f, F_SURFACE);
    char line[96];
    f_small(x + 20, y + 14, F_TEXT_3, "ESTIMATED LATENCY, PC TO TV");
    snprintf(line, sizeof line, "~%.0f ms", l.total);
    gfx_text_right(x + W - 20, y + 10, 1, F_TEXT, line);
    /* The parts, one bar, and their numbers under it. */
    const float bx = (float)x + 20.0f, bw = W - 40.0f, by = (float)y + 40.0f;
    const float parts[4] = { l.pc, l.net, l.decode, l.display };
    const uint32_t colours[4] = { 0xFF8B8B93, 0xFF3D8BFF, THEME_ACCENT, 0xFF9D74FF };
    static const char *const names[4] = { "PC", "Network", "Decode", "TV" };
    const float sum = l.pc + l.net + l.decode + l.display;
    float at = bx;
    for (int i = 0; i < 4; i++) {
        const float w = bw * parts[i] / sum;
        gfx_rect(at, by, w - (i < 3 ? 2.0f : 0.0f), 6.0f, 3.0f, colours[i]);
        at += w;
    }
    int32_t lx = x + 20;
    for (int i = 0; i < 4; i++) {
        gfx_circle((float)lx + 4.0f, by + 22.0f, 4.0f, colours[i]);
        snprintf(line, sizeof line, "%s %.0f", names[i], parts[i]);
        f_small(lx + 13, (int32_t)by + 14, F_TEXT_2, line);
        lx += 14 + f_small_width(line) + 18;
    }
    /* Whether decoding keeps up. */
    const bool fits = l.decode <= l.budget;
    snprintf(line, sizeof line, fits ? "Decoding keeps up: %.0f of %.1f ms a frame%s"
                                     : "Decoding is too slow: %.0f of %.1f ms a frame, frames will drop%s",
             l.decode, l.budget, on_wifi ? "  \xC2\xB7  Wi-Fi" : "");
    gfx_circle((float)x + 24.0f, (float)y + 84.0f, 4.0f, fits ? F_GOOD : THEME_WARN);
    f_small(x + 34, y + 76, fits ? F_TEXT_2 : THEME_WARN, line);
    if (lean) {
        gfx_circle((float)x + 24.0f, (float)y + 106.0f, 4.0f, THEME_WARN);
        f_small(x + 34, y + 98, THEME_WARN, "1080p60 needs a lean stream: Software encoder, ultrafast");
    }
}

static void f_settings(void)
{
    /* The sections, the current one marked. */
    static const Icon icons[FRESH_TABS] = { ICONS,      ICON_BITRATE, ICON_HOST, ICON_KEYBOARD, ICON_SPEAKER,
                                            ICON_THEME, ICON_TEST,    ICONS };
    for (int t = 1; t < FRESH_ABOUT; t++) {
        const float y = F_TOP + (float)((t - 1) * 52);
        const bool on = t == tab;
        if (on && f_in_rail) {   /* the cursor's here */
            f_focus(F_RAIL_X, y, F_RAIL_W, 48.0f, 14.0f);
        } else if (on) {   /* the section open */
            gfx_rect(F_RAIL_X, y, F_RAIL_W, 48.0f, 14.0f, F_SURFACE);
            gfx_rect(F_RAIL_X, y + 14.0f, 3.0f, 20.0f, 1.5f, THEME_ACCENT);
        }
        icon_draw(icons[t], F_RAIL_X + 16.0f, y + 9.0f, on ? F_TEXT : F_TEXT_3);
        gfx_text(F_RAIL_X + 16 + ICON_SIZE + 12, (int32_t)y + 14, 1, on ? F_TEXT : F_TEXT_3, tabs[t].name);
    }

    /* The rows. */
    const int count = tabs[tab].count;
    int32_t y = F_TOP;
    for (int i = 0; i < count; i++) {
        const int item = tabs[tab].items[i];
        const bool on = i == cursor[tab] && !f_in_rail;
        const char *heading = f_heading(tab, i);
        if (heading) {
            f_small(F_LIST_X + 20, y + 12, F_TEXT_3, heading);
            y += 36;
        }
        const bool forget = item == ITEM_FORGET;
        if (forget)   /* a line before it: it isn't a setting */
            gfx_rect(F_LIST_X + 20.0f, (float)y + 4.0f, F_LIST_W - 40.0f, 1.0f, 0.0f, 0xFF232326), y += 12;
        if (on && forget) {
            gfx_rect(F_LIST_X - 2.0f, (float)y - 2.0f, F_LIST_W + 4.0f, F_ROW_H + 4.0f, 18.0f, 0x8CFF6B6B);
            gfx_rect(F_LIST_X, (float)y, F_LIST_W, F_ROW_H, 16.0f, 0xFF2E1A1A);
            if (f_forget_holding) {
                const float k = (float)(gfx_frame() - f_forget_from) / F_FORGET_FRAMES;
                gfx_rect(F_LIST_X, (float)y, F_LIST_W * (k > 1.0f ? 1.0f : k), F_ROW_H, 16.0f, 0x66FF5C5C);
            }
        } else if (on) {
            f_focus(F_LIST_X, (float)y, F_LIST_W, F_ROW_H, 16.0f);
        }
        const int32_t ty = y + (F_ROW_H - 20) / 2, cy = y + F_ROW_H / 2, right = F_LIST_X + F_LIST_W - 20;
        gfx_text(F_LIST_X + 20, ty, 1, forget ? (config->host[0] ? F_ERROR_TEXT : F_TEXT_3) : on ? F_TEXT : F_TEXT_2,
                 item_label(item));
        if (forget) {
            /* nothing on the right: it's an action */
        } else if (is_toggle(item)) {
            f_switch(right, cy, f_toggle_on(item));
        } else {
            char value[64];
            bool warn;
            f_value_text(item, value, sizeof value, &warn);
            const uint32_t ink = warn ? THEME_WARN : on ? F_TEXT : F_TEXT_2;
            int32_t vr = right;
            if (item == ITEM_HOST) {
                gfx_text_right(vr, ty, 1, on ? F_TEXT : F_TEXT_3, "\xE2\x80\xBA");   /* U+203A */
                vr -= 22;
            } else if (on) {
                gfx_text_right(vr, ty, 1, F_TEXT_3, ARROW_RIGHT);
                vr -= gfx_text_width(ARROW_RIGHT, 1) + 12;
            }
            gfx_text_right(vr, ty, 1, ink, value);
            const int32_t left = vr - gfx_text_width(value, 1);
            if (on && item != ITEM_HOST)
                gfx_text_right(left - 12, ty, 1, F_TEXT_3, ARROW_LEFT);
            if (item == ITEM_THEME)
                gfx_circle((float)(left - (on ? 44 : 16)), (float)cy, 7.0f, THEME_ACCENT);
        }
        y += F_ROW_H + 2;
    }

    if (tab == 1)
        f_draw_latency(F_LIST_X, y + 8);

    /* The selected row, explained; on the sections, what the section holds. */
    const int item = tabs[tab].items[cursor[tab]];
    gfx_rect(F_PANEL_X, F_TOP, F_PANEL_W, F_PANEL_H, 22.0f, F_SURFACE);
    const int32_t px = F_PANEL_X + 28;
    if (f_in_rail) {
        gfx_text(px, F_TOP + 26, 2, F_TEXT, tabs[tab].name);
        f_lines(px, F_TOP + 26 + 40 + 18, F_TEXT_2, f_section_about[tab], 37, 26, false);
        return;
    }
    gfx_text(px, F_TOP + 26, 2, F_TEXT, item_label(item));
    const char *help, *warning;
    item_help(item, &help, &warning);
    help = f_places(help);
    warning = f_places(warning);
    int32_t hy = F_TOP + 26 + 40 + 18;
    hy += f_lines(px, hy, F_TEXT_2, help, 37, 26, false) * 26;
    if (warning) {
        hy += 18;
        gfx_circle((float)px + 5.0f, (float)hy + 8.0f, 5.0f, THEME_WARN);
        f_lines(px + 18, hy, THEME_WARN, warning, 42, 20, true);
    }
}

/* ---- connecting (ui_draw_progress, in the Fresh look) ---- */

/* Connecting, in motion: a track runs under the steps and fills on to the step under way (a light running along the
 * line on to the next step drawing itself), each step's tick pops in as it's done, the one under way spins, what it's
 * doing fades in as it changes, and the seconds count up by the title. A new connection starts it all again. */
void menu_draw_progress_fresh(uint32_t width, uint32_t height, const char *title, const char *subtitle,
                              const char *const *names, int count, int current, const char *detail)
{
    (void)width, (void)height;
    enum { MAX_STEPS = 8, POP = 14, DETAIL_FADE = 14 };
    const uint32_t now = gfx_frame();
    const float t = (float)now / 60.0f;
    /* A new connection: drawn again after a gap. */
    static uint32_t last, started, done_at[MAX_STEPS], detail_at;
    static int seen;
    static float fill;
    static char last_detail[160];
    if (now - last > 2) {
        started = now;
        seen = 0;
        fill = -1.0f;
        last_detail[0] = 0;
        for (int i = 0; i < MAX_STEPS; i++)
            done_at[i] = 0;
    }
    last = now;
    if (count > MAX_STEPS)
        count = MAX_STEPS;
    if (count < 1)
        return;
    for (; seen < current && seen < count; seen++)   /* steps just done: when, for their tick's pop */
        done_at[seen] = now;
    const char *what = detail && detail[0] ? detail : "";
    if (strcmp(what, last_detail) != 0) {
        snprintf(last_detail, sizeof last_detail, "%s", what);
        detail_at = now;
    }

    f_stars();
    f_picture(true, config->app_id, title, false);   /* as on Play */
    f_header(0, false);

    char line[160];
    int32_t y = 150;
    f_small(F_LEFT, y, THEME_ACCENT, "CONNECTING");
    snprintf(line, sizeof line, "%u s", (unsigned)((now - started) / 60));
    f_small(F_LEFT + f_small_width("CONNECTING") + 14, y, F_TEXT_3, line);
    y += 22;
    const uint32_t scale = gfx_text_width(title, 3) <= F_HERO_W ? 3 : 2;
    gfx_fit_width(line, sizeof line, title, scale, F_HERO_W);
    gfx_text(F_LEFT, y + (scale == 3 ? 0 : 10), scale, F_TEXT, line);
    y += 60 + 16;
    gfx_fit_width(line, sizeof line, subtitle, 1, F_HERO_W);
    gfx_text(F_LEFT, y, 1, F_TEXT_2, line);
    y += 26 + 30;

    /* Where each step's dot is. */
    int32_t widths[MAX_STEPS] = { 0 }, total = 0;
    float dots[MAX_STEPS] = { 0 };
    for (int i = 0; i < count; i++)
        total += widths[i] = 22 + 10 + f_small_width(names[i]);
    const int32_t gap = count > 1 ? (700 - total) / (count - 1) : 0;
    for (int i = 0, x = F_LEFT; i < count; i++) {
        dots[i] = (float)x + 11.0f;
        x += widths[i] + gap;
    }
    const float cy = (float)y + 11.0f;

    /* The track: grey along the whole row under the labels' gaps, green filled on to the step under way. */
    const int at = current < count ? current : count - 1;
    const float target = dots[at < 0 ? 0 : at];
    fill = fill < 0.0f ? dots[0] : fill + (target - fill) * 0.10f;
    for (int i = 0; i + 1 < count; i++) {
        const float a = dots[i] + 11.0f + (float)(widths[i] - 22) + 12.0f, b = dots[i + 1] - 22.0f;
        if (b <= a)
            continue;
        gfx_rect(a, cy - 1.5f, b - a, 3.0f, 1.5f, F_BORDER);
        const float done = fill >= b ? b - a : fill > a ? fill - a : 0.0f;
        if (done > 0.0f)
            gfx_rect(a, cy - 1.5f, done, 3.0f, 1.5f, F_GOOD);
        if (i == at && current < count) {
            /* A solid orange line that draws itself from the step under way to the next, over and over: it grows
             * (easing out), stays a moment, then fades and starts again. */
            const float cycle = fmodf(t / 1.4f, 1.0f);
            const float grow = cycle < 0.7f ? cycle / 0.7f : 1.0f, eased = 1.0f - (1.0f - grow) * (1.0f - grow);
            const float alpha = cycle < 0.85f ? 1.0f : 1.0f - (cycle - 0.85f) / 0.15f;
            if (eased > 0.0f)
                gfx_rect(a, cy - 1.5f, (b - a) * eased, 3.0f, 1.5f,
                         (uint32_t)(alpha * 255.0f) << 24 | (THEME_ACCENT & 0x00FFFFFF));
        }
    }

    for (int i = 0; i < count; i++) {
        const float dx = dots[i];
        const int32_t lx = (int32_t)dx + 21;
        if (i < current) {
            /* The tick pops: from small, a little past its size, then settles. */
            const uint32_t age = now - done_at[i];
            const float k = age >= POP ? 1.0f : (float)age / POP;
            const float r = 11.0f * (0.55f + 0.45f * k) + 3.5f * sinf(3.14159265f * k);
            if (age < POP * 2)   /* a ring that spreads and fades as it lands */
                gfx_circle(dx, cy, 11.0f + 10.0f * (float)age / (POP * 2),
                           (uint32_t)(70.0f * (1.0f - (float)age / (POP * 2))) << 24 | (F_GOOD & 0x00FFFFFF));
            gfx_circle(dx, cy, r, F_GOOD);
            gfx_text_center((int32_t)dx, (int32_t)cy - 10, 1, (uint32_t)(k * 255.0f) << 24 | (THEME_BG & 0x00FFFFFF),
                            "\xE2\x9C\x93");
            f_small(lx, (int32_t)cy - 8, F_TEXT_2, names[i]);
        } else if (i == current) {
            /* Under way: the spinner's orange dots, going round, nothing around them. */
            gfx_spinner(dx, cy, 9.0f, THEME_ACCENT);
            f_small(lx, (int32_t)cy - 8, F_TEXT, names[i]);
        } else {
            gfx_circle(dx, cy, 11.0f, F_TRACK);
            gfx_circle(dx, cy, 9.0f, THEME_BG);
            f_small(lx, (int32_t)cy - 8, 0xFF5A5A62, names[i]);
        }
    }

    /* What the step is doing: each new line fades in and rises a little. */
    y += 22 + 22;
    const uint32_t dage = now - detail_at;
    const float d = dage >= DETAIL_FADE ? 1.0f : (float)dage / DETAIL_FADE;
    gfx_fit_width(line, sizeof line, last_detail, 1, (int32_t)(F_HERO_W / 0.8f));
    f_small(F_LEFT, y + (int32_t)(6.0f * (1.0f - d)), (uint32_t)(d * 255.0f) << 24 | (F_TEXT_3 & 0x00FFFFFF), line);
    y += 34;
    snprintf(line, sizeof line, "Tip: while streaming, hold %s for a second to open the stream menu.",
             menu_combo_name(config->menu_combo));
    f_small(F_LEFT, y, F_TEXT_2, line);

    FApp shelf[CONFIG_RECENT];
    f_draw_shelf(shelf, f_shelf(shelf), false, 0);
    static const char *const labels[] = { "Exit" };
    ui_draw_hints("K", labels);
}

/* ---- the whole menu ---- */

static void fresh_draw_menu(uint32_t width, uint32_t height);
static bool f_startup_running(void);
static void f_draw_startup(uint32_t width, uint32_t height);

static void fresh_draw(uint32_t width, uint32_t height)
{
    fresh_draw_menu(width, height);
    if (f_startup_running())
        f_draw_startup(width, height);
}

static void fresh_draw_menu(uint32_t width, uint32_t height)
{
    if (finder_open || picker_open) {
        f_header(0, false);
        if (finder_open) {
            draw_finder();
            static const char *const labels[] = { "Choose", "Back", "Type an address", "Search again" };
            ui_draw_hints("ABXY", labels);
        } else {
            draw_picker();
            static const char *const labels[] = { "Choose", "Back", "Type a name", "Refresh" };
            ui_draw_hints("ABXY", labels);
        }
        return;
    }
    if (tab == TAB_PLAY)
        f_play();
    else if (tabs[tab].count == 0 && about_log && config->logging)
        draw_log();
    else if (tabs[tab].count == 0)
        draw_about(CONTENT_Y + 8);
    else
        f_settings();
    f_header(f_place(tab), tab == TAB_PLAY && config->host[0]);

    /* A new tab fades in. */
    static int drawn_tab = -1, faded_from = -1;
    static uint32_t tab_since;
    if (tab != drawn_tab) {
        tab_since = gfx_frame() - (drawn_tab < 0 ? FADE_FRAMES : 0);
        faded_from = drawn_tab;
        drawn_tab = tab;
    }
    const uint32_t age = gfx_frame() - tab_since;
    if (age < FADE_FRAMES) {   /* from section to section only the rows and the panel fade: the sections stay */
        const bool within = faded_from > TAB_PLAY && faded_from < FRESH_ABOUT && f_place(tab) == 1;
        const float fx = within ? F_LIST_X - 12.0f : 0.0f;
        gfx_rect(fx, CONTENT_Y - 30.0f, (float)width - fx, THEME_PANEL_BOTTOM - CONTENT_Y + 42.0f, 0.0f,
                 (255u * (FADE_FRAMES - age) / FADE_FRAMES) << 24 | (THEME_BG & 0x00FFFFFF));
    }

    if (tab == TAB_PLAY) {
        FApp shelf[CONFIG_RECENT];
        const int n = f_shelf(shelf);
        if (!config->host[0]) {
            static const char *const labels[] = { "Find your PC", NULL, "Settings", "Exit" };
            ui_draw_hints("ALRK", labels);
        } else if (f_row == F_ROW_SHELF && f_shelf_index < n) {
            static const char *const labels[] = { "Stream it", "Remove", "Back" };
            ui_draw_hints("AXB", labels);
        } else if (f_row == F_ROW_SHELF) {
            static const char *const labels[] = { "All apps", "Back" };
            ui_draw_hints("AB", labels);
        } else if (f_row == F_ROW_CHIP) {
            static const char *const labels[] = { "Quality settings", "Back" };
            ui_draw_hints("AB", labels);
        } else {
            const bool error = message[0] && message_is_error(message);
            const char *const labels[] = { error ? "Try again" : "Stream", "All apps", "Change PC", NULL, "Settings",
                                           "Exit" };
            ui_draw_hints("AYXLRK", labels);
        }
    } else if (tabs[tab].count == 0) {
        if (config->logging) {
            const char *const labels[] = { "Back", about_log ? "Credits" : "Show log", NULL, "Play \xC2\xB7 Settings" };
            ui_draw_hints("BYLR", labels);
        } else {
            static const char *const labels[] = { "Back", NULL, "Play \xC2\xB7 Settings" };
            ui_draw_hints("BLR", labels);
        }
    } else if (f_in_rail) {
        static const char *const labels[] = { "Open", "Play", NULL, "Play \xC2\xB7 About" };
        ui_draw_hints("ABLR", labels);
    } else {
        const int item = selected_item();
        const char *const labels[] = { item == ITEM_FORGET ? "Hold to forget"
                                       : item == ITEM_HOST ? "Choose"
                                       : is_toggle(item)   ? "Toggle"
                                                           : "Change",
                                       "Sections", NULL, "Play \xC2\xB7 About" };
        ui_draw_hints(item == ITEM_HOST || item == ITEM_FORGET || is_toggle(item) ? "ABLR" : "DBLR", labels);
    }
}

/* ---- the app's cover for the stream menu (overlay.c) ---- */

void menu_draw_app_cover(float x, float y, float w, float h)
{
    f_cover(config->app_id, config->app, x, y, w, h, 0xFFFFFFFF);
}

/* ---- the startup: stars, the moon rises and turns to a crescent, the name comes, then Play; any button skips ---- */

enum { F_STARTUP_FRAMES = 96 };   /* 1.6 s */
static int f_startup_state;       /* 0 not yet drawn, 1 running, 2 done */
static uint32_t f_startup_from;

/* Whether the startup is running; it runs only when the menu is drawn first just after the app opens. */
static bool f_startup_running(void)
{
    if (f_startup_state == 0) {
        f_startup_state = gfx_frame() < 180 ? 1 : 2;
        f_startup_from = gfx_frame();
    }
    if (f_startup_state == 1 && gfx_frame() - f_startup_from >= F_STARTUP_FRAMES)
        f_startup_state = 2;
    return f_startup_state == 1;
}

static bool fresh_startup_skip(const InputState *input)
{
    if (!fresh() || !f_startup_running())
        return false;
    for (int p = 0; p < INPUT_PADS; p++)
        if (input->pressed[p])
            f_startup_state = 2;
    return true;
}

static float f_ease(float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static void f_draw_startup(uint32_t width, uint32_t height)
{
    const float t = (float)(gfx_frame() - f_startup_from) / 60.0f;
    /* The background stays solid while the moon and the name go (1.15 to 1.35 s), then clears to show the menu. The
     * shadow that cuts the crescent is solid too, so the moon never shows whole as it fades. */
    const float veil = t < 1.35f ? 1.0f : 1.0f - (t - 1.35f) / 0.25f;
    const float fade = t < 1.15f ? 1.0f : t > 1.35f ? 0.0f : 1.0f - (t - 1.15f) / 0.2f;
    ui_draw_veil(width, height, veil);
    f_stars_at(f_ease(t / 0.12f) * veil, true);
    const float rise = f_ease((t - 0.05f) / 0.22f), phase = f_ease((t - 0.45f) / 0.36f);
    const float name = f_ease((t - 0.32f) / 0.2f);
    const float moon = rise * fade;
    if (moon > 0.0f) {
        const float r = 60.0f * (0.85f + 0.15f * rise), cx = 640.0f, cy = 270.0f + 40.0f * (1.0f - rise);
        gfx_circle(cx, cy, r, (uint32_t)(moon * 255.0f) << 24 | 0xF2EAD0);
        const float s = r / 60.0f;
        gfx_circle(cx + (154.0f - 116.0f * phase) * s, cy - 14.0f * s, 64.0f * s, THEME_BG);
        gfx_shadow(cx - 88.0f * s, cy - 88.0f * s, 176.0f * s, 176.0f * s, 88.0f * s, 70.0f,
                   (uint32_t)(0x26 * moon) << 24 | 0xF2EAD0);
    }
    const float n = name * fade;
    if (n > 0.0f) {
        const uint32_t na = (uint32_t)(n * 255.0f) << 24;
        const int32_t w = gfx_text_width("Moonlight 360", 2), x = 640 - w / 2, y = 372 + (int32_t)(12.0f * (1.0f - name));
        gfx_text(x, y, 2, na | (THEME_TEXT & 0x00FFFFFF), "Moonlight");
        gfx_text(x + gfx_text_width("Moonlight ", 2), y, 2, na | (THEME_ACCENT & 0x00FFFFFF), "360");
    }
}
