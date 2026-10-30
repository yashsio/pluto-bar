/* See LICENSE file for copyright and license details.
 *
 * pluto-bar is configured here, at compile time, just like dwm.
 * Edit this file, then run `make && ./pluto-bar`.
 */

/* appearance */
static const char font[] = "DejaVu Sans Mono 15px";
/* Labels only.  The tag at index i maps to workspace i + 1 under Hyprland
 * and to pluto's tag i + 1 under river.  dwm has nine tags; Hyprland
 * workspace 10 still exists (mod+0) but has no tag. */
static const char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

/* dwm prints the current layout here.  Hyprland has no dwm layouts, so this
 * is the fallback symbol; under river pluto's active layout symbol is shown
 * instead.  Set it to "" to leave the cell empty when there is no layout. */
static const char ltsymbol[] = "[]=";

/* Used when status is not piped into stdin. */
static const char clockfmt[] = "%a %b %d %H:%M";

/* colors[scheme][ColFg / ColBg] */
enum { SchemeNorm, SchemeSel };
static const char *colors[][2] = {
	/*               fg         bg        */
	// [SchemeNorm] = { "#bbbbbb", "#222222" },
	// [SchemeSel]  = { "#eeeeee", "#005577" },
	[SchemeNorm] = { "#d4be98", "#1d2021" },
	[SchemeSel]  = { "#282828", "#a9b665" },
};
