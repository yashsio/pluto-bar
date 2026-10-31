/* See LICENSE file for copyright and license details.
 *
 * pluto-status is configured here, at compile time, like dwm.
 * Edit this file, then run `make`.
 */

/* characters of "artist - title" to keep (UTF-8 aware) */
static const int MAX_SONG = 40;

/* ----------------------------------------------------------------------------
 * separators
 *
 * Every module is joined to the previous one by exactly one joiner:
 *   SEP - used if SHOW_SEPARATOR is 1 and the module has sep_before = 1
 *   GAP - used otherwise
 *
 * Set SHOW_SEPARATOR to 0 to disable all separators.
 * Give both the same padding if you want perfectly even spacing.
 * ------------------------------------------------------------------------- */
static const int SHOW_SEPARATOR = 1;
static const char *const SEP = " | ";
static const char *const GAP = " ";

/* ----------------------------------------------------------------------------
 * module layout
 *
 * Reorder this array to reorder the status line.
 *   enabled:  1 = always enabled, 0 = disabled, -1 = auto-detect
 *            (only shown if hardware/helper is found at startup)
 *   sep:      1 = put separator (SEP) before this module, 0 = put gap
 *   interval: seconds between updates; 0 = use sensible default for module
 *   symbol:   NULL = use default symbol, or set to override
 * ------------------------------------------------------------------------- */
enum {
	MOD_CPU,
	MOD_RAM,
	MOD_HOME,
	MOD_UPTIME,
	MOD_NETWORK,
	MOD_TEMP,
	MOD_VOL,
	MOD_MIC,
  MOD_SONG,
	MOD_BAT,
	MOD_BRIGHT,
	MOD_CLOCK,
	MOD_COUNT
};

struct module {
	int id;
	const char *symbol; /* NULL = module's built-in symbol */
	int enabled;        /* 1 = on, 0 = off, -1 = auto-detect */
	int sep_before;     /* 1 = separator before this module */
	int interval;       /* seconds, 0 = module default */
};

static const struct module MODULES[] = {
	{ MOD_CPU,     NULL,      1, 0, 2  },
	{ MOD_RAM,     NULL,      1, 0, 2  },
	{ MOD_HOME,    NULL,     -1, 1, 10 },
	{ MOD_UPTIME,  NULL,     -1, 1, 60 },
	{ MOD_NETWORK, NULL,     -1, 1, 5  },
	{ MOD_TEMP,    NULL,     -1, 1, 5  },
	{ MOD_VOL,     NULL,     -1, 1, 0  },
	{ MOD_MIC,     NULL,     -1, 0, 0  },
  { MOD_SONG,    NULL,     -1, 1, 0  },
	{ MOD_BAT,     NULL,     -1, 1, 10 },
	{ MOD_BRIGHT,  NULL,     -1, 0, 2  },
	{ MOD_CLOCK,   NULL,      1, 1, 60 },
};

/* battery thresholds (for icon selection) */
static const int BAT_FULL_THRESHOLD = 90;
static const int BAT_3_4_THRESHOLD = 70;
static const int BAT_HALF_THRESHOLD = 50;
static const int BAT_1_4_THRESHOLD = 25;

/* volume thresholds */
static const int VOL_LOW_THRESHOLD = 50;

/*
 * Legacy interval names kept for compatibility with existing logic; the
 * scheduler uses the per-module `interval`/defaults from MODULES[] above.
 * Tweak these only if you want to change the fallback values.
 */
static const int CPU_INTERVAL = 2;
static const int RAM_INTERVAL = 2;
static const int HOME_INTERVAL = 10;
static const int UPTIME_INTERVAL = 60;
static const int NETWORK_INTERVAL = 5;
static const int TEMP_INTERVAL = 5;
static const int BATTERY_INTERVAL = 10;
static const int BRIGHTNESS_INTERVAL = 2;
static const int CLOCK_INTERVAL = 60;

/* Only used when libpulse is unavailable or not ready and wpctl exists;
 * libpulse itself is event-driven. */
static const int AUDIO_INTERVAL = 1;

static const int COMMAND_TIMEOUT_MS = 500;

/* playerctl --follow has no exit event when a player disappears; verify
 * that a player still exists this often (milliseconds).  It is also used as
 * the polling fallback when playerctl --follow could not be started. */
static const int MUSIC_CHECK_MS = 3000;

/* clock format (the format string is passed to strftime) */
static const char *const CLOCKFMT = "%a %b %d %H:%M";

/* ----------------------------------------------------------------------------
 * symbols - customize these
 *
 * Symbols are written without trailing spaces on purpose.  Formatting adds
 * the single space between a symbol and its value, and the module joiner
 * adds the spacing between modules.
 * ------------------------------------------------------------------------- */

static const char *const SYM_CPU = "";
static const char *const SYM_RAM = "";
static const char *const SYM_TEMP = "";

static const char *const SYM_BAT_FULL = "󰁹";
static const char *const SYM_BAT_HIGH = "󰂁";
static const char *const SYM_BAT_HALF = "󰁿";
static const char *const SYM_BAT_LOW = "󰁻";
static const char *const SYM_CHARGING = "󰂄";

static const char *const SYM_VOL_MUTE = "";
static const char *const SYM_VOL_LOW = "";
static const char *const SYM_VOL_HIGH = "";

static const char *const SYM_MIC_ON = "";
static const char *const SYM_MIC_OFF = "";

static const char *const SYM_MUSIC_PAUSE = "";
static const char *const SYM_MUSIC_PLAY = "";

static const char *const SYM_NET_CONNECTED = "󰱓";
static const char *const SYM_NET_DISCONNECTED = "󰅛";

static const char *const SYM_HOME = "";

static const char *const SYM_BRIGHTNESS = "󰖨";

static const char *const SYM_UPTIME = "";
