/* SPDX-License-Identifier: MIT */
#include "terminal/theme.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "util.h"
#include "terminal/ansi.h"

enum tint_palette {
    TINT_PALETTE_NONE = 0,
    TINT_PALETTE_DARK,
    TINT_PALETTE_LIGHT,
};

struct role_style {
    const char *open;
    const char *close;
};

struct theme_preset {
    const char *name;
    enum tint_palette tint_palette;
    struct role_style roles[THEME_ROLE_COUNT];
};

/* Indexed colors remain compatible with terminals that do not support truecolor SGR. */
#define FG256(n) "\x1b[38;5;" #n "m"
/* Single combined sequences so table-cell replay can recognize a link by one escape. */
#define LINK256(n)  ANSI_CSI "4;38;5;" #n "m"
#define LINK256_OFF ANSI_CSI "24;39m"
#define ROLE_STYLE(open_sequence, close_sequence)                                                  \
    {.open = (open_sequence), .close = (close_sequence)}
#define COLOR_STYLE(open_sequence) ROLE_STYLE(open_sequence, ANSI_FG_DEFAULT)

/* Preserve identity by hue when retuning: warm accent marks the user, cool chrome marks the app,
 * and the stance and Markdown roles share a separate model tint. */
/* clang-format off */
static const struct theme_preset THEME_PRESETS[] = {
    {
        .name = "ansi",
        .roles = {
            COLOR_STYLE(ANSI_BRIGHT_MAGENTA),
            COLOR_STYLE(ANSI_CYAN),
            ROLE_STYLE(ANSI_DIM ANSI_CYAN, ANSI_FG_DEFAULT ANSI_BOLD_OFF),
            COLOR_STYLE(ANSI_CYAN),
            COLOR_STYLE(ANSI_CYAN),
            ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            ROLE_STYLE(ANSI_BOLD, ANSI_BOLD_OFF),
            ROLE_STYLE(ANSI_UNDERLINE, ANSI_UNDERLINE_OFF),
            COLOR_STYLE(ANSI_GREEN),
            COLOR_STYLE(ANSI_RED),
            COLOR_STYLE(ANSI_GREEN),
            COLOR_STYLE(ANSI_RED),
            COLOR_STYLE(ANSI_YELLOW),
        },
    },
    {
        .name = "dark",
        .tint_palette = TINT_PALETTE_DARK,
        .roles = {
            COLOR_STYLE(FG256(173)),
            COLOR_STYLE(FG256(37)),
            COLOR_STYLE(FG256(23)),
            COLOR_STYLE(FG256(38)),
            COLOR_STYLE(FG256(38)),
            COLOR_STYLE(FG256(31)),
            ROLE_STYLE(ANSI_BOLD FG256(38), ANSI_BOLD_OFF ANSI_FG_DEFAULT),
            ROLE_STYLE(LINK256(31), LINK256_OFF),
            COLOR_STYLE(FG256(34)),
            COLOR_STYLE(FG256(160)),
            COLOR_STYLE(FG256(28)),
            COLOR_STYLE(FG256(160)),
            COLOR_STYLE(FG256(178)),
        },
    },
    {
        .name = "light",
        .tint_palette = TINT_PALETTE_LIGHT,
        .roles = {
            COLOR_STYLE(FG256(130)),
            COLOR_STYLE(FG256(30)),
            COLOR_STYLE(FG256(37)),
            COLOR_STYLE(FG256(31)),
            COLOR_STYLE(FG256(31)),
            COLOR_STYLE(FG256(38)),
            ROLE_STYLE(ANSI_BOLD FG256(31), ANSI_BOLD_OFF ANSI_FG_DEFAULT),
            ROLE_STYLE(LINK256(38), LINK256_OFF),
            COLOR_STYLE(FG256(28)),
            COLOR_STYLE(FG256(124)),
            COLOR_STYLE(FG256(28)),
            COLOR_STYLE(FG256(160)),
            COLOR_STYLE(FG256(136)),
        },
    },
    {
        .name = "off",
        .roles = {
            {NULL, NULL},
            {NULL, NULL},
            ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            {NULL, NULL},
            {NULL, NULL},
            ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            ROLE_STYLE(ANSI_BOLD, ANSI_BOLD_OFF),
            ROLE_STYLE(ANSI_UNDERLINE, ANSI_UNDERLINE_OFF),
            {NULL, NULL},
            {NULL, NULL},
            {NULL, NULL},
            {NULL, NULL},
            {NULL, NULL},
        },
    },
};

struct tint {
    const char *name;
    const char *dark_opens[THEME_ROLE_COUNT];
    const char *light_opens[THEME_ROLE_COUNT];
};

/* Keep tints away from saturated status colors, the gray axis, and the warm user accent. */
/* clang-format off */
static const struct tint TINTS[] = {
    {.name = "teal"},
    {
        .name = "violet",
        .dark_opens = {
            NULL,
            NULL,
            NULL,
            FG256(140),
            FG256(140),
            FG256(97),
            ANSI_BOLD FG256(140),
            LINK256(97),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
        .light_opens = {
            NULL,
            NULL,
            NULL,
            FG256(97),
            FG256(97),
            FG256(140),
            ANSI_BOLD FG256(97),
            LINK256(140),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
    },
    {
        .name = "rose",
        .dark_opens = {
            NULL,
            NULL,
            NULL,
            FG256(168),
            FG256(168),
            FG256(132),
            ANSI_BOLD FG256(168),
            LINK256(132),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
        .light_opens = {
            NULL,
            NULL,
            NULL,
            FG256(132),
            FG256(132),
            FG256(168),
            ANSI_BOLD FG256(132),
            LINK256(168),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
    },
    {
        .name = "sage",
        .dark_opens = {
            NULL,
            NULL,
            NULL,
            FG256(114),
            FG256(114),
            FG256(71),
            ANSI_BOLD FG256(114),
            LINK256(71),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
        .light_opens = {
            NULL,
            NULL,
            NULL,
            FG256(71),
            FG256(71),
            FG256(114),
            ANSI_BOLD FG256(71),
            LINK256(114),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
        },
    },
};
/* clang-format on */

static const struct theme_preset *active_theme = &THEME_PRESETS[0];
static const struct tint *active_tint = &TINTS[0];

static const struct tint *tint_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(TINTS) / sizeof(TINTS[0]); i++) {
        if (strcasecmp(TINTS[i].name, name) == 0)
            return &TINTS[i];
    }
    return NULL;
}

static const char *const *tint_role_opens(const struct tint *tint)
{
    switch (active_theme->tint_palette) {
    case TINT_PALETTE_DARK:
        return tint->dark_opens;
    case TINT_PALETTE_LIGHT:
        return tint->light_opens;
    case TINT_PALETTE_NONE:
        return NULL;
    }
    return NULL;
}

static const char *resolve_role_open(const char *const *tint_opens, enum theme_role role)
{
    if (tint_opens && tint_opens[role])
        return tint_opens[role];
    const char *open = active_theme->roles[role].open;
    return open ? open : "";
}

const char *theme_open(enum theme_role role)
{
    return resolve_role_open(tint_role_opens(active_tint), role);
}

const char *theme_close(enum theme_role role)
{
    const char *close = active_theme->roles[role].close;
    return close ? close : "";
}

const char *theme_name(void)
{
    return active_theme->name;
}

const char *theme_tint_name(void)
{
    return active_tint->name;
}

const char *theme_tint_open(const char *name)
{
    const struct tint *tint = tint_find(name);
    if (!tint)
        return NULL;
    const char *const *tint_opens = tint_role_opens(tint);
    if (!tint_opens)
        return NULL;
    return resolve_role_open(tint_opens, THEME_STANCE);
}

/* COLORFGBG is the only common background hint that requires no terminal round trip. */
static const char *autodetect_theme(void)
{
    const char *no_color = getenv("NO_COLOR");
    if (no_color && *no_color)
        return "off";
    const char *term = getenv("TERM");
    if (!term || !*term || strcmp(term, "dumb") == 0)
        return "off";
    const char *colorterm = getenv("COLORTERM");
    if (!strstr(term, "256color") && !(colorterm && *colorterm))
        return "ansi";
    const char *color_fgbg = getenv("COLORFGBG");
    if (color_fgbg) {
        const char *background = strrchr(color_fgbg, ';');
        if (background && (strcmp(background + 1, "7") == 0 || strcmp(background + 1, "15") == 0))
            return "light";
    }
    return "dark";
}

int theme_set(const char *name)
{
    if (!name)
        return -1;
    if (strcasecmp(name, "auto") == 0)
        name = autodetect_theme();
    for (size_t i = 0; i < sizeof(THEME_PRESETS) / sizeof(THEME_PRESETS[0]); i++) {
        if (strcasecmp(THEME_PRESETS[i].name, name) == 0) {
            active_theme = &THEME_PRESETS[i];
            return 0;
        }
    }
    return -1;
}

int theme_tint_set(const char *name)
{
    const struct tint *tint = tint_find(name);
    if (!tint)
        return -1;
    active_tint = tint;
    return 0;
}

static int invalid_value_changed(char **last_value, const char *value)
{
    if (*last_value && strcmp(*last_value, value) == 0)
        return 0;
    free(*last_value);
    *last_value = xstrdup(value);
    return 1;
}

/* A runtime tint overrides the active preset's tint; lower tiers apply when the preset has none. */
static const char *configured_tint(void)
{
    const char *tint = NULL;
    if (strcmp(config_source("tint"), "run") == 0)
        tint = config_str("tint");
    if (!tint || !*tint) {
        const char *preset = config_str("preset");
        tint = (preset && *preset) ? config_preset_tint(preset) : NULL;
    }
    if (!tint || !*tint)
        tint = config_str("tint");
    return (tint && *tint) ? tint : "teal";
}

void theme_init(void)
{
    static char *last_invalid_theme;
    static char *last_invalid_tint;

    const char *theme = config_str("theme");
    if (!theme)
        theme = "auto";
    if (theme_set(theme) != 0) {
        theme_set("auto");
        if (invalid_value_changed(&last_invalid_theme, theme))
            hax_warn("unknown theme '%s' (expected auto, dark, light, ansi, or off)", theme);
    }

    const char *tint = configured_tint();
    if (theme_tint_set(tint) != 0) {
        theme_tint_set("teal");
        if (invalid_value_changed(&last_invalid_tint, tint))
            hax_warn("unknown tint '%s' (expected teal, violet, rose, or sage)", tint);
    }
}
