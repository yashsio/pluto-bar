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
 *
 * Compositor-specific code lives in backend-*.c behind backend.h.  This file
 * only draws the bar, owns the layer surfaces, and feeds compositor state
 * into the monitor list.
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
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "bar.h"
#include "backend.h"
#include "config.h"
#include "drw.h"

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

_Static_assert(LENGTH(tags) <= MAX_TAGS, "too many tags");

/* dwm's globals */
static int bh;                    /* bar height */
static int lrpad;                 /* left/right padding for text */
static char stext[512];           /* status text */
static Clr **scheme;
static int running = 1;
static int timer_fd = -1;

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

/* compositor backend (selected by backend.c) */
static int sync_requested;

/* status input */
static int stdin_open;
static int status_from_stdin;
static char stdin_line[512];
static size_t stdin_line_len;

static void draw_bar(Monitor *m);
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

/* Only the focused monitor displays the status text, so a status-only
 * update has no reason to repaint the other monitors. */
static void
draw_status(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		if (m->focused)
			draw_bar(m);
}

/* -------------------------------------------------------------------------
 * Services used by compositor backends
 * ------------------------------------------------------------------------- */

Monitor *
bar_monitors(void)
{
	return mons;
}

Monitor *
bar_find_monitor(const char *name)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		if (strcmp(m->name, name) == 0)
			return m;
	return NULL;
}

size_t
bar_tag_count(void)
{
	return LENGTH(tags);
}

void
bar_apply_monitor(Monitor *m, const struct MonitorState *state)
{
	m->tagset = state->tagset;
	m->occ = state->occ;
	m->focused = state->focused;
	m->floating = state->floating;
	snprintf(m->title, sizeof m->title, "%s",
	    state->title ? state->title : "");
	snprintf(m->ltsym, sizeof m->ltsym, "%s",
	    state->layout && state->layout[0] &&
	    strcmp(state->layout, "-") != 0 ? state->layout : ltsymbol);
}

void
bar_reset_monitor(Monitor *m)
{
	struct MonitorState state = { 0 };

	state.title = "";
	bar_apply_monitor(m, &state);
}

void
bar_reset_monitors(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		bar_reset_monitor(m);
}

void
bar_redraw(void)
{
	draw_all();
}

void
bar_request_sync(void)
{
	sync_requested = 1;
}

int
bar_take_sync_request(void)
{
	int requested = sync_requested;

	sync_requested = 0;
	return requested;
}

static void
bar_timer_arm(int ms)
{
	struct itimerspec its;

	memset(&its, 0, sizeof its);
	if (ms > 0) {
		its.it_value.tv_sec = ms / 1000;
		its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
		if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
			its.it_value.tv_nsec = 1;
	}
	if (timer_fd >= 0)
		timerfd_settime(timer_fd, 0, &its, NULL);
}

/*
 * The clock fallback needs a one-second tick.  Once status comes from
 * stdin there is nothing to tick; only a disconnected WM socket needs a
 * slow reconnect retry.  A zero interval leaves poll() asleep.
 */
void
bar_timer_update(void)
{
	if (timer_fd < 0)
		return;

	if (!status_from_stdin) {
		bar_timer_arm(1000);
		return;
	}
	if (backend && backend->needs_retry && backend->needs_retry()) {
		bar_timer_arm(5000);
		return;
	}
	bar_timer_arm(0);
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
	draw_status();
}

static void
read_status(void)
{
	char buf[1024];
	char prev[sizeof stext];
	ssize_t n;
	size_t i;
	int got = 0;

	snprintf(prev, sizeof prev, "%s", stext);

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
		int first = !status_from_stdin;

		status_from_stdin = 1;
		if (first)
			bar_timer_update();
		/* A status generator that prints every second would otherwise
		 * force a full-width repaint even when the text is unchanged. */
		if (strcmp(stext, prev) != 0)
			draw_status();
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

	if (!m->drw)
		return;

	for (i = 0; i < LENGTH(tags); i++) {
		w = m->tagw[i];
		if (x >= (int)tx && x < (int)(tx + w)) {
			if (!(m->tagset & (1u << i)) &&
			    backend && backend->tag_click)
				backend->tag_click(m, (int)i);
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
		w = m->tagw[i];
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
	size_t i, size;
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

	/* Tags and font are compile-time constants, so measure them once. */
	if (m->drw) {
		for (i = 0; i < LENGTH(tags); i++)
			m->tagw[i] = lrpad +
			    drw_text_width(m->drw, tags[i]);
	}
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

	backend_global(interface, version);

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
	if (backend && backend->tick)
		backend->tick();

	update_clock();
	bar_timer_update();
}

int
main(void)
{
	struct pollfd fds[5];
	int nfds, i, bfd, backend_idx;
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

	backend_select();
	if (!backend)
		fprintf(stderr, "pluto-bar: warning: no supported compositor "
		    "was detected; tags will not update\n");

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
	bar_timer_arm(1000);

	if (backend) {
		if (backend->connect && backend->connect() < 0)
			fprintf(stderr, "pluto-bar: warning: cannot connect to %s; "
			    "will keep retrying\n", backend->name);
	}

	update_clock();
	if (backend && backend->sync)
		backend->sync();

	while (running) {
		wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);

		nfds = 0;
		fds[nfds].fd = wl_display_get_fd(dpy);
		fds[nfds].events = POLLIN;
		nfds++;

		bfd = backend_fd();
		backend_idx = -1;
		if (bfd >= 0) {
			backend_idx = nfds;
			fds[nfds].fd = bfd;
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
			} else if (i == backend_idx) {
				if (backend->read_events)
					backend->read_events();
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

		if (bar_take_sync_request() && backend && backend->sync)
			backend->sync();
	}

	while (mons) {
		Monitor *next = mons->next;

		free_bar(mons);
		mons = next;
	}

	if (backend && backend->destroy)
		backend->destroy();
	close(timer_fd);

	for (i = 0; i < (int)LENGTH(colors); i++)
		drw_scm_free(scheme[i]);
	free(scheme);

	wl_display_disconnect(dpy);

	return 0;
}
