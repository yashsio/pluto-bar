/* See LICENSE file for copyright and license details.
 *
 * pluto-bar - a dwm-style status bar for Hyprland and river.
 *
 * dwm draws its bar with Xlib; Wayland has no such thing, so this is a
 * wlr-layer-shell client that does the same drawing with cairo/pango.
 * The layout is copied from dwm's drawbar(): tags on the left, a layout
 * symbol, the focused window title, and the status text on the right.
 *
 * State comes from Hyprland's IPC sockets ($HYPRLAND_INSTANCE_SIGNATURE) or,
 * under river 0.4+, from pluto's small status socket.  River 0.4 has no
 * client-facing status protocol; tags and window management belong to the
 * window manager, so the WM has to publish them.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "config.h"
#include "drw.h"

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct Monitor Monitor;
struct Monitor {
	char name[64];
	uint32_t global_id;
	struct wl_output *output;
	int scale;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	Drw *drw;
	int width, height;
	int configured;
	unsigned int tagset;      /* bit i = tag i is selected */
	unsigned int occ;         /* bit i = tag i has clients */
	int focused;
	int floating;
	char title[512];
	char ltsym[16];           /* river/pluto layout symbol, else ltsymbol */
	Monitor *next;
};

/* dwm's globals */
static int bh;                    /* bar height */
static int lrpad;                 /* left/right padding for text */
static char stext[512];           /* status text */
static Clr **scheme;
static int running = 1;

/* wayland */
static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static Monitor *mons;
static int ready;                 /* initial roundtrips done */

/* pointer */
static Monitor *ptr_mon;
static int ptr_x;

/* compositor backend */
enum Backend {
	BackendNone,
	BackendHyprland,
	BackendRiver,
};

static enum Backend backend;
static int river_available;       /* river globals seen on the registry */

/* hyprland ipc */
static int hypr_available;
static char hypr_sockdir[256];
static int hypr_fd = -1;
static char hypr_line[4096];
static size_t hypr_line_len;
static int dirty;

/* river/pluto ipc */
static char river_sockpath[256];
static int river_fd = -1;
static int river_in_snapshot;
static char river_line[4096];
static size_t river_line_len;

/* status input */
static int stdin_open;
static int status_from_stdin;
static char stdin_line[512];
static size_t stdin_line_len;

static void draw_bar(Monitor *m);
static void sync_state(void);
static void create_bar(Monitor *m);
static void free_bar(Monitor *m);
static const struct wl_seat_listener seat_listener;

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static void
draw_all(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		draw_bar(m);
}

/* -------------------------------------------------------------------------
 * Hyprland IPC
 * ------------------------------------------------------------------------- */

static void
hypr_init(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	int n;

	if (!runtime || !sig)
		return;

	n = snprintf(hypr_sockdir, sizeof hypr_sockdir, "%s/hypr/%s", runtime, sig);
	if (n < 0 || (size_t)n >= sizeof hypr_sockdir) {
		fprintf(stderr, "pluto-bar: Hyprland socket path is too long\n");
		return;
	}
	hypr_available = 1;
}

static int
hypr_fill_addr(struct sockaddr_un *addr, const char *sockname)
{
	size_t dlen = strlen(hypr_sockdir);
	size_t slen = strlen(sockname);

	memset(addr, 0, sizeof *addr);
	addr->sun_family = AF_UNIX;
	if (dlen + slen + 1 > sizeof addr->sun_path)
		return -1;
	memcpy(addr->sun_path, hypr_sockdir, dlen);
	memcpy(addr->sun_path + dlen, sockname, slen + 1);

	return 0;
}

static char *
hypr_request(const char *cmd)
{
	struct sockaddr_un addr;
	char *buf;
	size_t len = 0, cap = 4096;
	ssize_t n;
	int fd;

	if (!hypr_available)
		return NULL;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return NULL;

	if (hypr_fill_addr(&addr, "/.socket.sock") < 0 ||
	    connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return NULL;
	}

	if (write(fd, cmd, strlen(cmd)) < 0) {
		close(fd);
		return NULL;
	}

	buf = malloc(cap);
	if (!buf) {
		close(fd);
		return NULL;
	}

	for (;;) {
		if (len + 2048 + 1 > cap) {
			char *nb;

			cap *= 2;
			nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = nb;
		}
		n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		len += n;
	}
	close(fd);
	buf[len] = '\0';

	return buf;
}

struct ws_info {
	int id;
	int windows;
	char monitor[64];
};

struct mon_info {
	char name[64];
	int active;
	int focused;
};

static void
hypr_sync_state(void)
{
#define MAXWS 64
#define MAXMON 16
	char *wsreply, *monreply, *winreply;
	struct ws_info ws[MAXWS];
	struct mon_info mos[MAXMON];
	int nws = 0, nmos = 0, haswin = 0;
	int floating = 0;
	char title[512] = "";
	Monitor *m;
	int i;

	wsreply = hypr_request("workspaces");
	monreply = hypr_request("monitors");
	winreply = hypr_request("activewindow");

	if (wsreply) {
		char *save = NULL, *line;
		struct ws_info *cur = NULL;

		for (line = strtok_r(wsreply, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			int id;

			if (sscanf(line, "workspace ID %d", &id) == 1) {
				if (nws < MAXWS) {
					cur = &ws[nws++];
					memset(cur, 0, sizeof *cur);
					cur->id = id;
					char *p = strstr(line, " on monitor ");
					if (p)
						sscanf(p + 12, "%63[^:]", cur->monitor);
				} else {
					cur = NULL;
				}
			} else if (cur) {
				int w;

				if (sscanf(line, " windows: %d", &w) == 1)
					cur->windows = w;
			}
		}
	}

	if (monreply) {
		char *save = NULL, *line;
		struct mon_info *cur = NULL;

		for (line = strtok_r(monreply, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char name[64];
			int id, active;

			if (sscanf(line, "Monitor %63s (ID %d)", name, &id) == 2) {
				if (nmos < MAXMON) {
					cur = &mos[nmos++];
					memset(cur, 0, sizeof *cur);
					snprintf(cur->name, sizeof cur->name, "%s", name);
				} else {
					cur = NULL;
				}
			} else if (cur) {
				if (sscanf(line, " active workspace: %d", &active) == 1)
					cur->active = active;
				else if (strstr(line, "focused: yes"))
					cur->focused = 1;
			}
		}
	}

	if (winreply && strncmp(winreply, "Window ", 7) == 0) {
		char *save = NULL, *line;

		haswin = 1;
		for (line = strtok_r(winreply, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			if (sscanf(line, " floating: %d", &floating) == 1)
				continue;
			if (sscanf(line, " title: %511[^\n]", title) == 1)
				break;
		}
	}

	for (m = mons; m; m = m->next) {
		m->focused = 0;
		m->floating = 0;
		m->tagset = 0;
		m->occ = 0;
		m->title[0] = '\0';

		for (i = 0; i < nmos; i++) {
			if (strcmp(mos[i].name, m->name) != 0)
				continue;
			m->focused = mos[i].focused;
			if (mos[i].active >= 1 && mos[i].active <= (int)LENGTH(tags))
				m->tagset = 1u << (mos[i].active - 1);
		}

		for (i = 0; i < nws; i++) {
			int tag = ws[i].id - 1;

			if (tag < 0 || tag >= (int)LENGTH(tags))
				continue;
			if (ws[i].windows > 0 && strcmp(ws[i].monitor, m->name) == 0)
				m->occ |= 1u << tag;
		}

		if (m->focused && haswin) {
			snprintf(m->title, sizeof m->title, "%s", title);
			m->floating = floating;
		}
	}

	free(wsreply);
	free(monreply);
	free(winreply);

	draw_all();
#undef MAXWS
#undef MAXMON
}

static int
hypr_connect(void)
{
	struct sockaddr_un addr;
	int fd;

	if (!hypr_available)
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;

	if (hypr_fill_addr(&addr, "/.socket2.sock") < 0 ||
	    connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int
is_state_event(const char *line)
{
	static const char *names[] = {
		"workspace", "workspacev2",
		"focusedmon", "focusedmonv2",
		"createworkspace", "createworkspacev2",
		"destroyworkspace", "destroyworkspacev2",
		"moveworkspace", "moveworkspacev2",
		"renameworkspace", "activespecial", "activespecialv2",
		"activewindow", "activewindowv2", "windowtitle",
		"openwindow", "closewindow", "movewindow",
		"monitoradded", "monitoraddedv2", "monitorremoved",
		"configreloaded",
	};
	size_t n = strcspn(line, ">");
	size_t i;

	for (i = 0; i < LENGTH(names); i++)
		if (strlen(names[i]) == n && strncmp(line, names[i], n) == 0)
			return 1;

	return 0;
}

static void
hypr_read_events(void)
{
	char buf[4096];
	ssize_t n;
	size_t i;

	for (;;) {
		n = read(hypr_fd, buf, sizeof buf);
		if (n > 0) {
			for (i = 0; i < (size_t)n; i++) {
				if (buf[i] == '\n') {
					hypr_line[hypr_line_len] = '\0';
					if (hypr_line_len && is_state_event(hypr_line))
						dirty = 1;
					hypr_line_len = 0;
				} else if (hypr_line_len < sizeof hypr_line - 1) {
					hypr_line[hypr_line_len++] = buf[i];
				}
			}
		} else if (n == 0) {
			close(hypr_fd);
			hypr_fd = -1;
			hypr_line_len = 0;
			return;
		} else {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(hypr_fd);
			hypr_fd = -1;
			hypr_line_len = 0;
			return;
		}
	}
}

/* -------------------------------------------------------------------------
 * River 0.4 / pluto IPC
 * -------------------------------------------------------------------------
 *
 * River 0.4 has no client-facing status or control protocol: tags are
 * entirely the window manager's business.  pluto publishes per-monitor state
 * on a Unix socket and accepts a single "view" command.  The path matches
 * pluto's ipc.c:
 *
 *     $XDG_RUNTIME_DIR/pluto-<WAYLAND_DISPLAY>.sock
 *
 * and the line format is documented in pluto's ipc.h.
 */

static int
river_socket_path(char *buf, size_t size)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	char safe[128];
	size_t i, j = 0;
	int n;

	if (!runtime || !*runtime)
		runtime = "/tmp";
	if (!display || !*display)
		display = "wayland-0";

	for (i = 0; display[i] && j < sizeof safe - 1; i++) {
		unsigned char c = display[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			safe[j++] = c;
		else
			safe[j++] = '_';
	}
	safe[j] = '\0';

	n = snprintf(buf, size, "%s/pluto-%s.sock", runtime, safe);
	return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int
river_connect(void)
{
	struct sockaddr_un addr;
	int fd;

	if (!river_available)
		return -1;
	if (river_socket_path(river_sockpath, sizeof river_sockpath) < 0)
		return -1;
	if (strlen(river_sockpath) >= sizeof addr.sun_path)
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, river_sockpath, strlen(river_sockpath) + 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	river_fd = fd;
	river_line_len = 0;
	river_in_snapshot = 0;
	return 0;
}

static void
river_send(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;
	size_t off = 0;

	if (river_fd < 0 && river_connect() < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof buf)
		n = sizeof buf - 1;

	while (off < (size_t)n) {
		ssize_t written = send(river_fd, buf + off, (size_t)n - off,
		    MSG_NOSIGNAL);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(river_fd);
			river_fd = -1;
			return;
		}
		off += (size_t)written;
	}
}

static void
river_reset_monitor(Monitor *m)
{
	m->focused = 0;
	m->floating = 0;
	m->tagset = 0;
	m->occ = 0;
	m->title[0] = '\0';
	snprintf(m->ltsym, sizeof m->ltsym, "%s", ltsymbol);
}

static void
river_handle_line(char *line)
{
	Monitor *m;
	char name[64], layout[16];
	unsigned int tagset, occ;
	int focused, floating, off = 0;

	if (strcmp(line, "begin") == 0) {
		river_in_snapshot = 1;
		for (m = mons; m; m = m->next)
			river_reset_monitor(m);
		return;
	}
	if (strcmp(line, "end") == 0) {
		if (river_in_snapshot)
			draw_all();
		river_in_snapshot = 0;
		return;
	}
	if (!river_in_snapshot)
		return;
	if (sscanf(line, "monitor %63s %x %x %15s %d %d %n",
	    name, &tagset, &occ, layout, &focused, &floating, &off) < 6)
		return;

	for (m = mons; m; m = m->next) {
		if (strcmp(m->name, name) != 0)
			continue;
		m->tagset = tagset;
		m->occ = occ;
		m->focused = focused;
		m->floating = floating;
		snprintf(m->ltsym, sizeof m->ltsym, "%s",
		    strcmp(layout, "-") == 0 ? ltsymbol : layout);
		snprintf(m->title, sizeof m->title, "%s", line + off);
		return;
	}
}

static void
river_read_events(void)
{
	char buf[4096];
	ssize_t n;
	size_t i;

	for (;;) {
		n = read(river_fd, buf, sizeof buf);
		if (n > 0) {
			for (i = 0; i < (size_t)n; i++) {
				if (buf[i] == '\n') {
					river_line[river_line_len] = '\0';
					if (river_line_len)
						river_handle_line(river_line);
					river_line_len = 0;
				} else if (river_line_len < sizeof river_line - 1) {
					river_line[river_line_len++] = buf[i];
				}
			}
		} else if (n == 0) {
			close(river_fd);
			river_fd = -1;
			river_line_len = 0;
			river_in_snapshot = 0;
			return;
		} else {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(river_fd);
			river_fd = -1;
			river_line_len = 0;
			river_in_snapshot = 0;
			return;
		}
	}
}

static void
sync_state(void)
{
	if (backend == BackendHyprland)
		hypr_sync_state();
}

/* -------------------------------------------------------------------------
 * Status text: stdin acts like dwm's root window name. If nothing is
 * piped in, fall back to a clock.
 * ------------------------------------------------------------------------- */

static void
update_clock(void)
{
	char buf[256];
	time_t t;
	struct tm tm;

	if (status_from_stdin)
		return;

	t = time(NULL);
	localtime_r(&t, &tm);
	strftime(buf, sizeof buf, clockfmt, &tm);
	if (strcmp(buf, stext) == 0)
		return;
	snprintf(stext, sizeof stext, "%s", buf);
	draw_all();
}

static void
read_status(void)
{
	char buf[1024];
	ssize_t n;
	size_t i;
	int got = 0;

	for (;;) {
		n = read(STDIN_FILENO, buf, sizeof buf);
		if (n > 0) {
			for (i = 0; i < (size_t)n; i++) {
				if (buf[i] == '\n') {
					stdin_line[stdin_line_len] = '\0';
					if (stdin_line_len &&
					    stdin_line[stdin_line_len - 1] == '\r')
						stdin_line[stdin_line_len - 1] = '\0';
					snprintf(stext, sizeof stext, "%s", stdin_line);
					stdin_line_len = 0;
					got = 1;
				} else if (stdin_line_len < sizeof stdin_line - 1) {
					stdin_line[stdin_line_len++] = buf[i];
				}
			}
		} else if (n == 0) {
			stdin_open = 0;
			if (stdin_line_len) {
				stdin_line[stdin_line_len] = '\0';
				snprintf(stext, sizeof stext, "%s", stdin_line);
				stdin_line_len = 0;
				got = 1;
			}
			break;
		} else {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				stdin_open = 0;
			break;
		}
	}

	if (got) {
		status_from_stdin = 1;
		draw_all();
	}
}

/* -------------------------------------------------------------------------
 * Wayland
 * ------------------------------------------------------------------------- */

static Monitor *
monitor_for_surface(struct wl_surface *surface)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		if (m->surface == surface)
			return m;

	return NULL;
}

static void
tag_click(Monitor *m, int x)
{
	unsigned int i, tx = 0, w;
	char cmd[128];

	if (!m->drw)
		return;

	for (i = 0; i < LENGTH(tags); i++) {
		w = lrpad + drw_text_width(m->drw, tags[i]);
		if (x >= (int)tx && x < (int)(tx + w)) {
			if (!(m->tagset & (1u << i))) {
				if (backend == BackendRiver) {
					/* river 0.4: ask the WM to view this tag. */
					river_send("view %s %x\n", m->name,
					    1u << i);
					return;
				}

				char *r;

				/* Hyprland's Lua config evaluates a dispatch argument
				 * as Lua; fall back to the legacy syntax on failure. */
				snprintf(cmd, sizeof cmd,
				    "dispatch hl.dsp.focus({ workspace = %u })", i + 1);
				r = hypr_request(cmd);
				if (r && strstr(r, "error")) {
					free(r);
					snprintf(cmd, sizeof cmd,
					    "dispatch workspace %u", i + 1);
					r = hypr_request(cmd);
				}
				free(r);
			}
			return;
		}
		tx += w;
	}
}

static void
draw_bar(Monitor *m)
{
	Drw *drw;
	const char *ltsym;
	int x, w, tw = 0;
	int boxs, boxw;
	unsigned int i;

	if (!m->configured || !m->drw || !m->surface || !m->buffer)
		return;

	drw = m->drw;
	boxs = drw->fonth / 9;
	boxw = drw->fonth / 6 + 2;

	/* dwm relies on the cells covering everything; clear first anyway */
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, m->width, m->height, 1, 1);

	/* draw status first so it can be overdrawn by tags later */
	if (m->focused && stext[0]) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		tw = drw_text_width(drw, stext) + 2; /* 2px right padding */
		drw_text(drw, m->width - tw, 0, tw, m->height, 0, stext, 0);
	}

	x = 0;
	for (i = 0; i < LENGTH(tags); i++) {
		w = lrpad + drw_text_width(drw, tags[i]);
		drw_setscheme(drw,
		    scheme[(m->tagset & (1u << i)) ? SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, w, m->height, lrpad / 2, tags[i], 0);
		if (m->occ & (1u << i))
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
			    m->focused && (m->tagset & (1u << i)), 0);
		x += w;
	}

	ltsym = m->ltsym[0] ? m->ltsym : ltsymbol;
	w = lrpad + drw_text_width(drw, ltsym);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, m->height, lrpad / 2, ltsym, 0);

	if ((w = m->width - tw - x) > bh) {
		if (m->focused && m->title[0]) {
			drw_setscheme(drw, scheme[SchemeSel]);
			drw_text(drw, x, 0, w, m->height, lrpad / 2, m->title, 0);
			if (m->floating)
				drw_rect(drw, x + boxs, boxs, boxw, boxw, 0, 0);
		} else {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, w, m->height, 1, 1);
		}
	}

	wl_surface_attach(m->surface, m->buffer, 0, 0);
	wl_surface_damage(m->surface, 0, 0, m->width, m->height);
	wl_surface_commit(m->surface);
}

static void
free_buffer(Monitor *m)
{
	if (m->drw) {
		drw_free(m->drw);
		m->drw = NULL;
	}
	if (m->buffer) {
		wl_buffer_destroy(m->buffer);
		m->buffer = NULL;
	}
	if (m->data) {
		munmap(m->data, m->size);
		m->data = NULL;
	}
}

static void
create_buffer(Monitor *m)
{
	int pw, ph, stride;
	size_t size;
	struct wl_shm_pool *pool;
	void *data;
	int fd;

	free_buffer(m);

	if (!m->surface || m->width <= 0 || m->height <= 0)
		return;

	pw = m->width * m->scale;
	ph = m->height * m->scale;
	stride = pw * 4;
	size = (size_t)stride * (size_t)ph;

	fd = memfd_create("pluto-bar", MFD_CLOEXEC);
	if (fd < 0)
		die("pluto-bar: memfd_create: %s\n", strerror(errno));
	if (ftruncate(fd, (off_t)size) < 0)
		die("pluto-bar: ftruncate: %s\n", strerror(errno));

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		die("pluto-bar: mmap: %s\n", strerror(errno));
	}

	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	m->buffer = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride,
	    WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	m->data = data;
	m->size = size;

	wl_surface_set_buffer_scale(m->surface, m->scale);
	m->drw = drw_create(m->width, m->height, m->scale, data, stride, font);
}

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
    uint32_t serial, uint32_t width, uint32_t height)
{
	Monitor *m = data;

	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (width > 0)
		m->width = width;
	if (height > 0)
		m->height = height;
	m->configured = 1;
	create_buffer(m);
	draw_bar(m);
}

static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	Monitor *m = data;

	zwlr_layer_surface_v1_destroy(layer);
	wl_surface_destroy(m->surface);
	m->layer = NULL;
	m->surface = NULL;
	m->configured = 0;
	if (ptr_mon == m)
		ptr_mon = NULL;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void
create_bar(Monitor *m)
{
	if (!ready || !compositor || !layer_shell || m->surface || !m->output)
		return;

	m->surface = wl_compositor_create_surface(compositor);
	if (!m->surface)
		return;

	m->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, m->surface,
	    m->output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "pluto-bar");
	if (!m->layer) {
		wl_surface_destroy(m->surface);
		m->surface = NULL;
		return;
	}

	zwlr_layer_surface_v1_add_listener(m->layer, &layer_listener, m);
	zwlr_layer_surface_v1_set_size(m->layer, 0, bh);
	zwlr_layer_surface_v1_set_anchor(m->layer,
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(m->layer, bh);
	zwlr_layer_surface_v1_set_keyboard_interactivity(m->layer,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(m->surface);
	wl_display_flush(dpy);
}

static void
free_bar(Monitor *m)
{
	free_buffer(m);
	if (m->layer)
		zwlr_layer_surface_v1_destroy(m->layer);
	if (m->surface)
		wl_surface_destroy(m->surface);
	if (m->output)
		wl_output_destroy(m->output);
	if (ptr_mon == m)
		ptr_mon = NULL;
	free(m);
}

/* output events */
static void
output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char *make, const char *model, int32_t transform)
{
	(void)data; (void)output; (void)x; (void)y;
	(void)physical_width; (void)physical_height; (void)subpixel;
	(void)make; (void)model; (void)transform;
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags,
    int32_t width, int32_t height, int32_t refresh)
{
	(void)data; (void)output; (void)flags;
	(void)width; (void)height; (void)refresh;
}

static void
output_done(void *data, struct wl_output *output)
{
	Monitor *m = data;

	(void)output;
	if (!m->surface)
		create_bar(m);
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	Monitor *m = data;

	(void)output;
	if (factor <= 0 || factor == m->scale)
		return;

	m->scale = factor;
	if (m->surface && m->configured) {
		create_buffer(m);
		draw_bar(m);
	}
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
	Monitor *m = data;

	(void)output;
	snprintf(m->name, sizeof m->name, "%s", name);
}

static void
output_description(void *data, struct wl_output *output, const char *description)
{
	(void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version)
{
	(void)data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
		    MIN(version, 4));
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(reg, name,
		    &zwlr_layer_shell_v1_interface, MIN(version, 4));
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		if (!seat) {
			seat = wl_registry_bind(reg, name, &wl_seat_interface,
			    MIN(version, 5));
			wl_seat_add_listener(seat, &seat_listener, NULL);
		}
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		Monitor *m = calloc(1, sizeof *m);

		if (!m)
			die("pluto-bar: out of memory\n");
		m->global_id = name;
		m->scale = 1;
		snprintf(m->ltsym, sizeof m->ltsym, "%s", ltsymbol);
		m->output = wl_registry_bind(reg, name, &wl_output_interface,
		    MIN(version, 4));
		wl_output_add_listener(m->output, &output_listener, m);
		m->next = mons;
		mons = m;
	} else if (strcmp(interface, "river_layer_shell_v1") == 0 ||
	    strcmp(interface, "river_window_manager_v1") == 0) {
		/* River 0.4: state comes from the WM over its IPC socket. */
		river_available = 1;
	}
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	Monitor **mp = &mons;

	(void)data;
	(void)reg;

	while (*mp) {
		Monitor *m = *mp;

		if (m->global_id == name) {
			*mp = m->next;
			free_bar(m);
			return;
		}
		mp = &m->next;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* seat / pointer */
static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data; (void)pointer; (void)serial; (void)sy;

	ptr_mon = monitor_for_surface(surface);
	ptr_x = wl_fixed_to_int(sx);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface)
{
	(void)data; (void)pointer; (void)serial; (void)surface;
	ptr_mon = NULL;
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data; (void)pointer; (void)time; (void)sy;
	ptr_x = wl_fixed_to_int(sx);
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)pointer; (void)serial; (void)time;

	if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	if (ptr_mon)
		tag_click(ptr_mon, ptr_x);
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void
pointer_frame(void *data, struct wl_pointer *pointer)
{
	(void)data; (void)pointer;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
	(void)data; (void)pointer; (void)source;
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis)
{
	(void)data; (void)pointer; (void)time; (void)axis;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
    int32_t discrete)
{
	(void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
		ptr_mon = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

static void
tick(void)
{
	if (backend == BackendHyprland && !hypr_fd && hypr_available) {
		hypr_fd = hypr_connect();
		if (hypr_fd >= 0)
			dirty = 1;
	}

	if (backend == BackendRiver && river_fd < 0)
		river_connect();

	update_clock();
}

int
main(void)
{
	struct itimerspec its;
	struct pollfd fds[5];
	int timer_fd, nfds, i;
	Monitor *m;
	int font_h;

	setlocale(LC_ALL, "");

	drw_font_metrics(font, &font_h, NULL);
	lrpad = font_h;
	bh = font_h + 2;

	scheme = calloc(LENGTH(colors), sizeof(Clr *));
	if (!scheme)
		die("pluto-bar: out of memory\n");
	for (i = 0; i < (int)LENGTH(colors); i++)
		scheme[i] = drw_scm_create(colors[i], 2);

	hypr_init();

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("pluto-bar: cannot connect to Wayland display\n");

	registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);

	/* first roundtrip: globals, second: output names and scales */
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);

	if (!compositor || !shm || !layer_shell)
		die("pluto-bar: compositor is missing wl_compositor, wl_shm or wlr-layer-shell\n");

	if (hypr_available)
		backend = BackendHyprland;
	else if (river_available)
		backend = BackendRiver;
	else {
		backend = BackendNone;
		fprintf(stderr, "pluto-bar: warning: neither Hyprland nor river "
		    "was detected; tags will not update\n");
	}

	ready = 1;
	i = 0;
	for (m = mons; m; m = m->next) {
		if (!m->name[0])
			snprintf(m->name, sizeof m->name, "MONITOR-%d", i++);
		create_bar(m);
	}

	if (!isatty(STDIN_FILENO)) {
		int flags = fcntl(STDIN_FILENO, F_GETFL);

		if (flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0)
			stdin_open = 1;
	}

	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (timer_fd < 0)
		die("pluto-bar: timerfd_create: %s\n", strerror(errno));
	its.it_value.tv_sec = 1;
	its.it_value.tv_nsec = 0;
	its.it_interval = its.it_value;
	if (timerfd_settime(timer_fd, 0, &its, NULL) < 0)
		die("pluto-bar: timerfd_settime: %s\n", strerror(errno));

	if (backend == BackendHyprland) {
		hypr_fd = hypr_connect();
		if (hypr_fd < 0)
			fprintf(stderr, "pluto-bar: warning: cannot connect to Hyprland event socket\n");
	} else if (backend == BackendRiver) {
		if (river_connect() < 0)
			fprintf(stderr, "pluto-bar: warning: cannot connect to "
			    "pluto IPC socket; will keep retrying\n");
	}

	update_clock();
	sync_state();

	while (running) {
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		nfds = 0;
		fds[nfds].fd = wl_display_get_fd(dpy);
		fds[nfds].events = POLLIN;
		nfds++;

		if (hypr_fd >= 0) {
			fds[nfds].fd = hypr_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (river_fd >= 0) {
			fds[nfds].fd = river_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (stdin_open) {
			fds[nfds].fd = STDIN_FILENO;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		fds[nfds].fd = timer_fd;
		fds[nfds].events = POLLIN;
		nfds++;

		if (poll(fds, nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			die("pluto-bar: poll: %s\n", strerror(errno));
		}

		for (i = 0; i < nfds; i++) {
			if (!fds[i].revents)
				continue;

			if (fds[i].fd == wl_display_get_fd(dpy)) {
				if (wl_display_dispatch(dpy) < 0)
					running = 0;
			} else if (hypr_fd >= 0 && fds[i].fd == hypr_fd) {
				hypr_read_events();
			} else if (river_fd >= 0 && fds[i].fd == river_fd) {
				river_read_events();
			} else if (fds[i].fd == timer_fd) {
				uint64_t exp;

				if (read(timer_fd, &exp, sizeof exp) < 0 &&
				    errno != EAGAIN && errno != EINTR)
					fprintf(stderr, "pluto-bar: timerfd read failed\n");
				tick();
			} else if (stdin_open && fds[i].fd == STDIN_FILENO) {
				read_status();
			}
		}

		if (dirty) {
			dirty = 0;
			sync_state();
		}
	}

	while (mons) {
		Monitor *next = mons->next;

		free_bar(mons);
		mons = next;
	}

	if (hypr_fd >= 0)
		close(hypr_fd);
	if (river_fd >= 0)
		close(river_fd);
	close(timer_fd);

	for (i = 0; i < (int)LENGTH(colors); i++)
		drw_scm_free(scheme[i]);
	free(scheme);

	wl_display_disconnect(dpy);

	return 0;
}
