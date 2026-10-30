/* See LICENSE file for copyright and license details.
 *
 * River 0.4 / pluto backend.
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
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend.h"
#include "bar.h"

static int river_available;       /* river/pluto globals seen on the registry */
static char river_sockpath[256];
static int river_fd = -1;
static int river_in_snapshot;
static char river_line[4096];
static size_t river_line_len;

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
river_open(void)
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

	if (river_fd < 0 && river_open() < 0)
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
			bar_timer_update();
			return;
		}
		off += (size_t)written;
	}
}

static void
river_handle_line(char *line)
{
	Monitor *m;
	struct MonitorState st;
	char name[64], layout[16];
	unsigned int tagset, occ;
	int focused, floating, off = 0;

	if (strcmp(line, "begin") == 0) {
		river_in_snapshot = 1;
		bar_reset_monitors();
		return;
	}
	if (strcmp(line, "end") == 0) {
		if (river_in_snapshot)
			bar_redraw();
		river_in_snapshot = 0;
		return;
	}
	if (!river_in_snapshot)
		return;
	if (sscanf(line, "monitor %63s %x %x %15s %d %d %n",
	    name, &tagset, &occ, layout, &focused, &floating, &off) < 6)
		return;

	m = bar_find_monitor(name);
	if (!m)
		return;

	memset(&st, 0, sizeof st);
	st.tagset = tagset;
	st.occ = occ;
	st.layout = layout;
	st.focused = focused;
	st.floating = floating;
	st.title = line + off;
	bar_apply_monitor(m, &st);
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
			bar_timer_update();
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
			bar_timer_update();
			return;
		}
	}
}

static void
river_tag_click(Monitor *m, int tag)
{
	river_send("view %s %x\n", m->name, 1u << tag);
}

/* -------------------------------------------------------------------------
 * backend interface
 * ------------------------------------------------------------------------- */

static int
river_detect(void)
{
	return river_available;
}

static void
river_global(const char *interface, uint32_t version)
{
	(void)version;

	if (strcmp(interface, "river_layer_shell_v1") == 0 ||
	    strcmp(interface, "river_window_manager_v1") == 0)
		river_available = 1;
}

static int
river_connect(void)
{
	return river_open();
}

static int
river_get_fd(void)
{
	return river_fd;
}

static void
river_tick(void)
{
	if (river_fd < 0)
		river_open();
}

static int
river_needs_retry(void)
{
	return river_fd < 0;
}

static void
river_destroy(void)
{
	if (river_fd >= 0)
		close(river_fd);
	river_fd = -1;
}

const struct Backend backend_river = {
	.name = "river/pluto",
	.detect = river_detect,
	.global = river_global,
	.connect = river_connect,
	.fd = river_get_fd,
	.read_events = river_read_events,
	.sync = NULL,
	.tag_click = river_tag_click,
	.tick = river_tick,
	.needs_retry = river_needs_retry,
	.destroy = river_destroy,
};
