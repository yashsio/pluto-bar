/* See LICENSE file for copyright and license details.
 *
 * pluto-status is configured here, at compile time, just like status.sh was.
 * Edit this file, then run `make`.
 */

/* characters of "artist - title" to keep (UTF-8 aware) */
static const int MAX_SONG = 40;

/* ----------------------------------------------------------------------------
 * module layout
 *
 * Every module is joined to the previous one by exactly one of:
 *
 *   SEP - used when SHOW_SEPARATOR is 1 and the module's SEPARATE_* flag is 1
 *   GAP - used otherwise
 *
 * The idea is that modules are written without any leading or trailing
 * spaces.  The joiner adds the spacing, so every module gets the same gap,
 * and a missing module (dead battery, no player, ...) cannot leave a double
 * separator behind.
 *
 * To turn separators off completely, set SHOW_SEPARATOR to 0.  To put a
 * separator between every module, set SEPARATE_ALL to 1.  Otherwise each
 * module follows its own SEPARATE_* flag below.  What counts as "equal
 * spacing" is controlled by SEP and GAP themselves; give both the same
 * padding (two spaces by default).
 * ------------------------------------------------------------------------- */

static const int SHOW_SEPARATOR = 1;
static const int SEPARATE_ALL = 0;
static const char *const SEP = "  |  ";
static const char *const GAP = "  ";

/* 1 = put SEP before this module, 0 = put GAP before it.
 * The defaults reproduce the old grouping: CPU+RAM, wifi+BT, volume+mic,
 * battery+power+brightness. */
static const int SEPARATE_CPU = 0;
static const int SEPARATE_RAM = 0;
static const int SEPARATE_HOME = 1;
static const int SEPARATE_UPTIME = 1;
static const int SEPARATE_WIFI = 1;
static const int SEPARATE_BT = 0;
static const int SEPARATE_TEMP = 1;
static const int SEPARATE_VOLUME = 1;
static const int SEPARATE_MIC = 0;
static const int SEPARATE_BATTERY = 1;
static const int SEPARATE_POWER_PROFILE = 0;
static const int SEPARATE_BRIGHTNESS = 0;
static const int SEPARATE_MUSIC = 1;
static const int SEPARATE_CLOCK = 1;

/* feature toggles */
enum {
	ENABLE_SONG = 1,
	ENABLE_CPU = 1,
	ENABLE_RAM = 1,
	ENABLE_VOL = 1,
	ENABLE_MIC = 1,
	ENABLE_BAT = 1,
	ENABLE_CLOCK = 1,
	ENABLE_TEMP = 1,
	ENABLE_WIFI = 1,
	ENABLE_BT = 1,
	ENABLE_POWER_PROFILE = 1,
	TOGGLE_HOME = 1,
	TOGGLE_BRIGHTNESS = 1,
	TOGGLE_UPTIME = 1,
};

/* battery thresholds (for icon selection) */
static const int BAT_FULL_THRESHOLD = 90;
static const int BAT_3_4_THRESHOLD = 70;
static const int BAT_HALF_THRESHOLD = 50;
static const int BAT_1_4_THRESHOLD = 25;

/* volume thresholds */
static const int VOL_LOW_THRESHOLD = 50;

/* Per-module refresh intervals, in seconds.  The scheduler sleeps until the
 * next module is due and then refreshes only that module, so a slow-changing
 * value does not force a read every second.
 *
 * CPU and RAM default to 2 seconds to keep the loop at 0.5 Hz.  Set them
 * to 1 for a 1 Hz status line, or higher if you prefer fewer wakeups.
 * CLOCK_INTERVAL is aligned to the wall clock; keep it at 1 if CLOCKFMT
 * contains seconds.
 *
 * Battery, temperature, backlight and power profile are discovered once at
 * startup.  If no hardware is found they are removed from the schedule
 * entirely, so their intervals cannot create wakeups.  Audio is scheduled
 * only when libpulse is ready or wpctl exists.  Wifi is the exception: it
 * keeps its "off" field and is rediscovered every WIFI_RETRY_INTERVAL
 * seconds so a hotplugged interface still appears.  Interfaces are found
 * through /proc/net/wireless and /sys/class/net, so cfg80211 drivers without
 * WEXT support are detected too; if the link is associated but the signal
 * level is unknown, the full-strength symbol is used. */
static const int CPU_INTERVAL = 2;
static const int RAM_INTERVAL = 2;
static const int HOME_INTERVAL = 10;
static const int UPTIME_INTERVAL = 60;
static const int WIFI_INTERVAL = 5;
/* When no wireless interface exists, rediscover it this often instead of
 * waking the whole status process for an absent module. */
static const int WIFI_RETRY_INTERVAL = 30;
static const int TEMP_INTERVAL = 5;
static const int BATTERY_INTERVAL = 10;
static const int BRIGHTNESS_INTERVAL = 2;
static const int CLOCK_INTERVAL = 60;

/* Only used when libpulse is unavailable or not ready and wpctl exists;
 * libpulse itself is event-driven, so volume and microphone changes wake the
 * status immediately. */
static const int AUDIO_INTERVAL = 1;

/* how often the few unavoidable helper processes are queried, in seconds */
static const int BLUETOOTH_INTERVAL = 30;
static const int POWER_PROFILE_INTERVAL = 30;
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

static const char *const SYM_POWER_BALANCED = "";
static const char *const SYM_POWER_PERFORMANCE = "󰡴";
static const char *const SYM_POWER_POWERSAVE = "󰡳";

static const char *const SYM_WIFI_4 = "󰤨";
static const char *const SYM_WIFI_3 = "󰤥";
static const char *const SYM_WIFI_2 = "󰤢";
static const char *const SYM_WIFI_1 = "󰤟";
static const char *const SYM_WIFI_OFF = "󰤭";

static const char *const SYM_BT = "󰂱";
static const char *const SYM_BT_OFF = "󰂲";

static const char *const SYM_HOME = "";

static const char *const SYM_BRIGHTNESS = "󰖨";

static const char *const SYM_UPTIME = "";
