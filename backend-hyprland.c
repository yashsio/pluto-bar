/* See LICENSE file for copyright and license details.
 *
 * Hyprland backend: state comes from Hyprland's command and event sockets.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend.h"
#include "bar.h"

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

static int hypr_available;
static char hypr_sockdir[256];
static int hypr_fd = -1;
static char hypr_line[4096];
static size_t hypr_line_len;

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
hypr_sync(void)
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
  		char monitor[64];
  
  		if (sscanf(line, "workspace %d (%d) on monitor %63s", &id, &id, monitor) >= 2 ||
  		    sscanf(line, "workspace %d on monitor %63s", &id, monitor) == 2) {
  			if (nws < MAXWS) {
  				cur = &ws[nws++];
  				memset(cur, 0, sizeof *cur);
  				cur->id = id;
  				sscanf(monitor, "%63[^:]", cur->monitor);
  			} else {
  				cur = NULL;
  			}
  		} else if (cur) {
  			int w;
  
  			if (sscanf(line, "	windows: %d", &w) == 1)
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

	for (m = bar_monitors(); m; m = m->next) {
		struct MonitorState st;

		memset(&st, 0, sizeof st);
		st.title = "";

		for (i = 0; i < nmos; i++) {
			if (strcmp(mos[i].name, m->name) != 0)
				continue;
			st.focused = mos[i].focused;
			if (mos[i].active >= 1 &&
			    mos[i].active <= (int)bar_tag_count())
				st.tagset = 1u << (mos[i].active - 1);
		}

		for (i = 0; i < nws; i++) {
			int tag = ws[i].id - 1;

			if (tag < 0 || tag >= (int)bar_tag_count())
				continue;
			if (ws[i].windows > 0 && strcmp(ws[i].monitor, m->name) == 0)
				st.occ |= 1u << tag;
		}

		if (st.focused && haswin) {
			st.title = title;
			st.floating = floating;
		}

		bar_apply_monitor(m, &st);
	}

	free(wsreply);
	free(monreply);
	free(winreply);

	bar_redraw();
#undef MAXWS
#undef MAXMON
}

static int
hypr_open(void)
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
						bar_request_sync();
					hypr_line_len = 0;
				} else if (hypr_line_len < sizeof hypr_line - 1) {
					hypr_line[hypr_line_len++] = buf[i];
				}
			}
		} else if (n == 0) {
			close(hypr_fd);
			hypr_fd = -1;
			hypr_line_len = 0;
			bar_timer_update();
			return;
		} else {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close(hypr_fd);
			hypr_fd = -1;
			hypr_line_len = 0;
			bar_timer_update();
			return;
		}
	}
}

static void
hypr_tag_click(Monitor *m, int tag)
{
	char cmd[128];
	char *r;

	(void)m;

	/* Hyprland's Lua config evaluates a dispatch argument as Lua; fall
	 * back to the legacy syntax on failure. */
	snprintf(cmd, sizeof cmd,
	    "dispatch hl.dsp.focus({ workspace = %d })", tag + 1);
	r = hypr_request(cmd);
	if (r && strstr(r, "error")) {
		free(r);
		snprintf(cmd, sizeof cmd, "dispatch workspace %d", tag + 1);
		r = hypr_request(cmd);
	}
	free(r);
}

/* -------------------------------------------------------------------------
 * backend interface
 * ------------------------------------------------------------------------- */

static int
hypr_detect(void)
{
	hypr_init();
	return hypr_available;
}

static void
hypr_global(const char *interface, uint32_t version)
{
	(void)interface;
	(void)version;
}

static int
hypr_connect(void)
{
	hypr_fd = hypr_open();
	return hypr_fd < 0 ? -1 : 0;
}

static int
hypr_get_fd(void)
{
	return hypr_fd;
}

static void
hypr_tick(void)
{
	if (hypr_fd < 0 && hypr_available) {
		if (hypr_connect() == 0)
			bar_request_sync();
	}
}

static int
hypr_needs_retry(void)
{
	return hypr_available && hypr_fd < 0;
}

static void
hypr_destroy(void)
{
	if (hypr_fd >= 0)
		close(hypr_fd);
	hypr_fd = -1;
}

const struct Backend backend_hyprland = {
	.name = "Hyprland",
	.detect = hypr_detect,
	.global = hypr_global,
	.connect = hypr_connect,
	.fd = hypr_get_fd,
	.read_events = hypr_read_events,
	.sync = hypr_sync,
	.tag_click = hypr_tag_click,
	.tick = hypr_tick,
	.needs_retry = hypr_needs_retry,
	.destroy = hypr_destroy,
};
