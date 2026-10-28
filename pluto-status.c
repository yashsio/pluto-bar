/* See LICENSE file for copyright and license details.
 *
 * pluto-status - the status feed for pluto-bar.
 *
 * This is a drop-in replacement for status.sh.  Each module has its own
 * compile-time refresh interval; the scheduler sleeps until the next module is
 * due, and writes a line only when the text changes, with the same fields,
 * order, glyphs and separators as the old script.  Modules with no hardware
 * are dropped from the schedule instead of being probed forever.
 *
 * /proc and /sys are read directly, so CPU, RAM, disk, uptime, temperature,
 * battery, wifi, brightness and the clock do not fork.  Audio is followed
 * through libpulse when it is available, and music through one persistent
 * `playerctl --follow`; both fall back to polling when unavailable.  The
 * remaining helpers (bluetoothctl, powerprofilesctl) are cached.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

#include "status-config.h"

extern char **environ;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VERSION "0.1"
#define OUTSIZE 2048

struct outbuf {
	char s[OUTSIZE];
	size_t len;
};

/* discovered hardware */
static char battery_dir[PATH_MAX];
static char temp_path[PATH_MAX];
static char wlan_if[64];
static char backlight_path[PATH_MAX];
static int backlight_max = -1;

/* optional helpers */
static int have_playerctl;
static int have_wpctl;
static int have_powerprofilesctl;
static int have_brightnessctl;
static int have_bluetoothctl;
static char playerctl_path[PATH_MAX];
static char wpctl_path[PATH_MAX];
static char powerprofilesctl_path[PATH_MAX];
static char brightnessctl_path[PATH_MAX];
static char bluetoothctl_path[PATH_MAX];

/* cpu delta */
static unsigned long long prev_cpu_total;
static unsigned long long prev_cpu_idle;
static int cpu_sampled;

/* per-module sampling state */
static long long cpu_next;
static int cpu_value;
static int cpu_valid;

static long long ram_next;
static int ram_value;
static int ram_valid;

static long long home_next;
static unsigned long long home_value;
static int home_valid;

static long long uptime_next;
static unsigned long long uptime_value;
static int uptime_valid;

static long long wifi_sample_next;
static const char *wifi_symbol;
static int wifi_valid;
static int wifi_failures;
static long long wifi_next;

static long long temp_next;
static int temp_value;
static int temp_valid;
static int temp_missing;

static int bt_on;
static int bt_valid;
static long long bt_next;

static long long battery_next;
static int battery_pct;
static int battery_charging;
static int battery_valid;
static int battery_missing;

static char pp_value[64];
static const char *power_symbol;
static int power_valid;
static long long power_next;
static int power_missing;

static long long brightness_next;
static int brightness_value;
static int brightness_valid;
static int brightness_missing;

static long long audio_next;
static int sink_value = -1;
static int sink_muted;
static int sink_valid;
static int source_value = -1;
static int source_muted;
static int source_valid;

static char clock_value[128];
static int clock_valid;
static long long clock_next;

/* event wakeup pipe for audio and music threads */
static int wake_pipe[2] = { -1, -1 };

/* libpulse event state (optional) */
#ifdef HAVE_PULSE
static pa_mainloop *pulse_ml;
static pa_context *pulse_ctx;
static atomic_int pulse_server_ready;
static atomic_int pulse_sink_ready;
static atomic_int pulse_source_ready;
static atomic_int pulse_ready;
static atomic_int pulse_dead;
static atomic_int pulse_sink_pct = -1;
static atomic_int pulse_sink_muted;
static atomic_int pulse_source_pct = -1;
static atomic_int pulse_source_muted;
static int pulse_started;
static char pulse_sink_name[256];
static char pulse_source_name[256];
#endif

/* playerctl --follow state */
static pthread_mutex_t music_lock = PTHREAD_MUTEX_INITIALIZER;
static char music_state[32];
static char music_song[512];
static int music_follow;
static long long music_next_check;
static long long music_poll_next;
static char music_poll_state[32];
static char music_poll_song[512];
static volatile sig_atomic_t player_pid;

static const char *power_profile_symbol(const char *profile);
static void utf8_truncate(char *s, size_t max);
static void trim_ws(char *s);
static void wake_main(void);
static int music_cached(void);

/* -------------------------------------------------------------------------
 * output buffer
 * ------------------------------------------------------------------------- */

static void
out_raw(struct outbuf *b, const char *s)
{
	size_t n = strlen(s);

	if (b->len >= sizeof b->s - 1)
		return;
	if (n > sizeof b->s - 1 - b->len)
		n = sizeof b->s - 1 - b->len;
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

/*
 * Append one module.  The module text is trimmed first, then joined to the
 * previous module with SEP or GAP.  This is the only place that inserts
 * spacing between modules, so the gap is identical for every pair.
 */
static void
out_module(struct outbuf *b, int separate, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;

	trim_ws(tmp);
	if (!tmp[0])
		return;

	if (b->len)
		out_raw(b, SHOW_SEPARATOR && (SEPARATE_ALL || separate) ? SEP
		    : GAP);
	out_raw(b, tmp);
}

static int
emit(const struct outbuf *b)
{
	char line[OUTSIZE + 1];
	size_t len = b->len;
	size_t off = 0;

	memcpy(line, b->s, len);
	line[len++] = '\n';

	while (off < len) {
		ssize_t n = write(STDOUT_FILENO, line + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * small helpers
 * ------------------------------------------------------------------------- */

static void
copy_str(char *dst, size_t size, const char *src)
{
	size_t n;

	if (!size)
		return;
	n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int
path_join(char *dst, size_t size, const char *dir, const char *name)
{
	size_t dlen = strlen(dir);
	size_t nlen = strlen(name);

	if (dlen + nlen + 2 > size)
		return -1;
	memcpy(dst, dir, dlen);
	dst[dlen] = '/';
	memcpy(dst + dlen + 1, name, nlen + 1);
	return 0;
}

static int
read_file(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t n;
	size_t len = 0;

	if (!size)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	while (len + 1 < size) {
		n = read(fd, buf + len, size - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	buf[len] = '\0';

	return (int)len;
}

static int
parse_int(const char *s)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || v < INT_MIN || v > INT_MAX)
		return -1;

	return (int)v;
}

static int
read_int_file(const char *path)
{
	char buf[64];

	if (read_file(path, buf, sizeof buf) < 0)
		return -1;

	return parse_int(buf);
}

static void
trim_ws(char *s)
{
	char *p = s;
	size_t n;

	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);

	n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

static void
chomp(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

static long long
now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;

	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
sleep_ms(long ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static long long
interval_ms(int seconds)
{
	return seconds > 0 ? (long long)seconds * 1000LL : 1000LL;
}

static int
module_due(long long *next, int interval, long long now)
{
	if (now < *next)
		return 0;

	*next = now + interval_ms(interval);
	return 1;
}

static void
set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
init_wake_pipe(void)
{
	if (pipe(wake_pipe) < 0) {
		wake_pipe[0] = -1;
		wake_pipe[1] = -1;
		return;
	}

	set_nonblock(wake_pipe[0]);
	set_nonblock(wake_pipe[1]);
	fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
}

static void
wake_main(void)
{
	char c = 1;

	if (wake_pipe[1] < 0)
		return;
	if (write(wake_pipe[1], &c, 1) < 0)
		return;
}

static void
drain_wake_pipe(void)
{
	char buf[64];

	if (wake_pipe[0] < 0)
		return;
	while (read(wake_pipe[0], buf, sizeof buf) > 0)
		;
}

static void
wait_for_wake(long long next)
{
	struct pollfd pfd;
	long long now;
	int timeout;

	for (;;) {
		now = now_ms();
		if (next <= now)
			return;

		timeout = next - now > INT_MAX ? INT_MAX :
		    (int)(next - now);
		if (wake_pipe[0] < 0) {
			/* No event pipe: keep event-driven modules responsive. */
			if (timeout > 1000)
				timeout = 1000;
			while (poll(NULL, 0, timeout) < 0 && errno == EINTR)
				;
			return;
		}

		pfd.fd = wake_pipe[0];
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, timeout) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}

		drain_wake_pipe();
		return;
	}
}

static void
cleanup_player(void)
{
	if (player_pid > 0)
		kill((pid_t)player_pid, SIGTERM);
}

static void
term_handler(int sig)
{
	cleanup_player();
	_exit(128 + sig);
}

static int
find_cmd_path(const char *cmd, char *dst, size_t size)
{
	const char *path;
	const char *p;
	char buf[PATH_MAX];

	if (strchr(cmd, '/')) {
		if (access(cmd, X_OK) != 0)
			return -1;
		copy_str(dst, size, cmd);
		return 0;
	}

	path = getenv("PATH");
	if (!path)
		return -1;

	for (p = path;; p = strchr(p, ':') + 1) {
		const char *colon = strchr(p, ':');
		size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
		size_t clen = strlen(cmd);

		if (dlen == 0) {
			if (clen + 3 > sizeof buf) {
				if (!colon)
					break;
				continue;
			}
			buf[0] = '.';
			buf[1] = '/';
			memcpy(buf + 2, cmd, clen + 1);
		} else {
			if (dlen + clen + 2 > sizeof buf) {
				if (!colon)
					break;
				continue;
			}
			memcpy(buf, p, dlen);
			buf[dlen] = '/';
			memcpy(buf + dlen + 1, cmd, clen + 1);
		}
		if (access(buf, X_OK) == 0) {
			copy_str(dst, size, buf);
			return 0;
		}
		if (!colon)
			break;
	}

	return -1;
}

/*
 * Run a command, capture stdout and return 0 only if it exited successfully.
 * The timeout is enforced here rather than with an external `timeout`
 * process, so even the unavoidable helpers cost exactly one fork each.
 */
static int
run_cmd(char *const argv[], char *out, size_t outsz, int timeout_ms)
{
	int fds[2];
	pid_t pid;
	posix_spawn_file_actions_t fa;
	int status = 0;
	int done = 0;
	int waited;
	long long start;
	size_t len = 0;

	if (!outsz)
		return -1;
	out[0] = '\0';

	if (pipe(fds) < 0)
		return -1;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	if (fds[1] != STDOUT_FILENO) {
		posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&fa, fds[1]);
	}
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
	    O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
	    O_WRONLY, 0);

	if (posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	{
		int flags = fcntl(fds[0], F_GETFL);

		if (flags >= 0)
			fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	}

	start = now_ms();
	while (!done) {
		struct pollfd pfd;
		int remaining = timeout_ms - (int)(now_ms() - start);
		int pr;

		if (remaining <= 0)
			break;

		pfd.fd = fds[0];
		pfd.events = POLLIN;
		pfd.revents = 0;
		pr = poll(&pfd, 1, remaining);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			break;

		for (;;) {
			char sink[512];
			char *dst;
			size_t cap;
			int discard = 0;
			ssize_t n;

			if (len < outsz - 1) {
				dst = out + len;
				cap = outsz - 1 - len;
			} else {
				dst = sink;
				cap = sizeof sink;
				discard = 1;
			}

			n = read(fds[0], dst, cap);
			if (n > 0) {
				if (!discard)
					len += (size_t)n;
				continue;
			}
			if (n == 0) {
				done = 1;
				break;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			done = 1;
			break;
		}
	}
	out[len] = '\0';
	close(fds[0]);

	if (!done)
		kill(pid, SIGKILL);

	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);

	if (waited < 0 || !done)
		return -1;

	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * hardware discovery
 * ------------------------------------------------------------------------- */

static void
find_battery(void)
{
	glob_t g;
	size_t i;

	battery_dir[0] = '\0';
	if (glob("/sys/class/power_supply/BAT*", 0, NULL, &g) != 0)
		return;

	for (i = 0; i < g.gl_pathc; i++) {
		char path[PATH_MAX];

		if (path_join(path, sizeof path, g.gl_pathv[i], "capacity") < 0)
			continue;
		if (access(path, R_OK) != 0)
			continue;
		copy_str(battery_dir, sizeof battery_dir, g.gl_pathv[i]);
		break;
	}

	globfree(&g);
}

static void
find_temp(void)
{
	glob_t g;
	size_t i;
	int best = -1;

	temp_path[0] = '\0';
	if (glob("/sys/class/thermal/thermal_zone*", 0, NULL, &g) != 0)
		return;

	for (i = 0; i < g.gl_pathc; i++) {
		char type[64];
		char path[PATH_MAX];
		int score;

		if (path_join(path, sizeof path, g.gl_pathv[i], "type") < 0)
			continue;
		if (read_file(path, type, sizeof type) < 0)
			continue;
		trim_ws(type);

		if (strstr(type, "x86_pkg_temp"))
			score = 100;
		else if (strstr(type, "coretemp") || strstr(type, "k10temp"))
			score = 90;
		else if (strstr(type, "cpu"))
			score = 80;
		else if (strstr(type, "soc"))
			score = 50;
		else if (strstr(type, "acpitz"))
			score = 1;
		else
			score = 10;

		if (path_join(path, sizeof path, g.gl_pathv[i], "temp") < 0)
			continue;
		if (access(path, R_OK) != 0)
			continue;

		if (score > best) {
			best = score;
			copy_str(temp_path, sizeof temp_path, path);
		}
	}

	globfree(&g);
}

/*
 * A cfg80211 driver can expose a wireless interface without adding a WEXT
 * entry to /proc/net/wireless.  sysfs still has the net device, so use both
 * sources and prefer an associated link when more than one is present.
 */
static int
wifi_iface_exists(const char *name)
{
	char dir[PATH_MAX];

	if (path_join(dir, sizeof dir, "/sys/class/net", name) < 0)
		return 0;

	return access(dir, F_OK) == 0;
}

static int
wifi_iface_up(const char *name)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	char buf[64];
	int carrier;

	if (path_join(dir, sizeof dir, "/sys/class/net", name) < 0)
		return 0;

	carrier = -1;
	if (path_join(path, sizeof path, dir, "carrier") == 0)
		carrier = read_int_file(path);
	if (carrier >= 0)
		return carrier == 1;

	if (path_join(path, sizeof path, dir, "operstate") == 0 &&
	    read_file(path, buf, sizeof buf) >= 0) {
		trim_ws(buf);
		if (strcmp(buf, "up") == 0)
			return 1;
		if (strcmp(buf, "down") == 0 || strcmp(buf, "dormant") == 0 ||
		    strcmp(buf, "lowerlayerdown") == 0)
			return 0;
	}

	/* WEXT sysfs has an explicit association state on some drivers. */
	if (path_join(path, sizeof path, dir, "wireless/status") == 0 &&
	    read_file(path, buf, sizeof buf) >= 0) {
		trim_ws(buf);
		return strcmp(buf, "Connected") == 0;
	}

	return 0;
}

static int
wifi_iface_is_wireless(const char *dir)
{
	char path[PATH_MAX];

	if (path_join(path, sizeof path, dir, "wireless") == 0 &&
	    access(path, F_OK) == 0)
		return 1;
	if (path_join(path, sizeof path, dir, "phy80211") == 0 &&
	    access(path, F_OK) == 0)
		return 1;

	return 0;
}

static void
find_wifi(void)
{
	char buf[16384];
	char first[64] = "";
	char up[64] = "";
	char *save = NULL;
	char *line;
	glob_t g;
	size_t i;

	wlan_if[0] = '\0';

	/* /proc/net/wireless also carries the signal levels read below. */
	if (read_file("/proc/net/wireless", buf, sizeof buf) >= 0) {
		for (line = strtok_r(buf, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char *colon = strchr(line, ':');

			/* The header has no colon.  Do not trust sscanf's
			 * return value here: a scanset can succeed before the
			 * literal ':' fails, which made the header look like
			 * an interface. */
			if (!colon)
				continue;
			*colon = '\0';
			trim_ws(line);
			if (!line[0] || strpbrk(line, " \t|"))
				continue;

			if (!first[0])
				copy_str(first, sizeof first, line);
			if (!up[0] && wifi_iface_up(line))
				copy_str(up, sizeof up, line);
		}
	}

	if (glob("/sys/class/net/*", 0, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc; i++) {
			const char *name = strrchr(g.gl_pathv[i], '/');

			if (!name || !name[1])
				continue;
			name++;
			if (!wifi_iface_is_wireless(g.gl_pathv[i]))
				continue;

			if (!first[0])
				copy_str(first, sizeof first, name);
			if (!up[0] && wifi_iface_up(name))
				copy_str(up, sizeof up, name);
		}
		globfree(&g);
	}

	copy_str(wlan_if, sizeof wlan_if, up[0] ? up : first);
}

static void
find_backlight(void)
{
	glob_t g;
	size_t i;

	backlight_path[0] = '\0';
	backlight_max = -1;
	if (glob("/sys/class/backlight/*", 0, NULL, &g) != 0)
		return;

	for (i = 0; i < g.gl_pathc; i++) {
		char bpath[PATH_MAX];
		char mpath[PATH_MAX];
		int max;

		if (path_join(mpath, sizeof mpath, g.gl_pathv[i],
		    "max_brightness") < 0)
			continue;
		max = read_int_file(mpath);
		if (max <= 0)
			continue;
		if (path_join(bpath, sizeof bpath, g.gl_pathv[i], "brightness") < 0)
			continue;
		if (access(bpath, R_OK) != 0)
			continue;

		copy_str(backlight_path, sizeof backlight_path, g.gl_pathv[i]);
		backlight_max = max;
		break;
	}

	globfree(&g);
}

/* -------------------------------------------------------------------------
 * readings
 * ------------------------------------------------------------------------- */

static int
get_cpu(void)
{
	char buf[4096];
	unsigned long long u, n, s, i, w, irq, sirq, st;
	unsigned long long total, idle, dt, di;

	if (read_file("/proc/stat", buf, sizeof buf) < 0)
		return -1;
	if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	    &u, &n, &s, &i, &w, &irq, &sirq, &st) != 8)
		return -1;

	total = u + n + s + i + w + irq + sirq + st;
	idle = i + w;

	if (!cpu_sampled) {
		prev_cpu_total = total;
		prev_cpu_idle = idle;
		cpu_sampled = 1;
		return 0;
	}

	if (total < prev_cpu_total || idle < prev_cpu_idle) {
		prev_cpu_total = total;
		prev_cpu_idle = idle;
		return 0;
	}

	dt = total - prev_cpu_total;
	di = idle - prev_cpu_idle;
	prev_cpu_total = total;
	prev_cpu_idle = idle;

	if (!dt || di > dt)
		return 0;

	return (int)(100 * (dt - di) / dt);
}

static int
get_ram(void)
{
	char buf[16384];
	char *save = NULL;
	char *line;
	unsigned long long total = 0, avail = 0;
	unsigned long long free_mem = 0, buffers = 0, cached = 0;

	if (read_file("/proc/meminfo", buf, sizeof buf) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		unsigned long long v;

		if (sscanf(line, "MemTotal: %llu", &v) == 1)
			total = v;
		else if (sscanf(line, "MemAvailable: %llu", &v) == 1)
			avail = v;
		else if (sscanf(line, "MemFree: %llu", &v) == 1)
			free_mem = v;
		else if (sscanf(line, "Buffers: %llu", &v) == 1)
			buffers = v;
		else if (sscanf(line, "Cached: %llu", &v) == 1)
			cached = v;
	}

	if (!total)
		return -1;
	if (!avail)
		avail = free_mem + buffers + cached;
	if (avail > total)
		avail = total;

	return (int)(100 * (total - avail) / total);
}

static int
get_home_gb(unsigned long long *gb)
{
	struct statvfs vfs;
	const char *home = getenv("HOME");
	unsigned long long frsize;
	unsigned long long used;

	if (!home || !*home)
		home = "/";
	if (statvfs(home, &vfs) != 0)
		return -1;

	frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	if (!frsize || vfs.f_blocks < vfs.f_bfree)
		return -1;

	used = (unsigned long long)(vfs.f_blocks - vfs.f_bfree) *
	    (unsigned long long)frsize;
	*gb = used / (1024ULL * 1024ULL * 1024ULL);

	return 0;
}

static int
get_uptime_hours(unsigned long long *hours)
{
	struct sysinfo si;

	if (sysinfo(&si) != 0)
		return -1;

	*hours = (unsigned long long)si.uptime / 3600;
	return 0;
}

static int
get_temp(int *temp)
{
	int raw;

	if (!temp_path[0])
		return -1;

	raw = read_int_file(temp_path);
	if (raw < 0)
		return -1;

	*temp = raw / 1000;
	return 0;
}

static int
get_battery(int *pct, int *charging)
{
	char path[PATH_MAX];
	char status[64];

	if (!battery_dir[0])
		return -1;
	if (path_join(path, sizeof path, battery_dir, "capacity") < 0)
		return -1;

	*pct = read_int_file(path);
	if (*pct < 0)
		return -1;

	*charging = 0;
	if (path_join(path, sizeof path, battery_dir, "status") == 0 &&
	    read_file(path, status, sizeof status) >= 0) {
		trim_ws(status);
		*charging = strcmp(status, "Charging") == 0;
	}

	return 0;
}

static int
get_brightness(void)
{
	char path[PATH_MAX];
	long long pct;
	int value;

	if (backlight_path[0]) {
		if (path_join(path, sizeof path, backlight_path, "brightness") < 0)
			return -1;
		value = read_int_file(path);
	} else if (have_brightnessctl && backlight_max > 0) {
		char out[64];
		char *argv[] = { brightnessctl_path, "get", NULL };

		if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) < 0)
			return -1;
		value = parse_int(out);
	} else {
		return -1;
	}

	if (value < 0 || backlight_max <= 0)
		return -1;

	pct = (long long)value * 100 / backlight_max;
	if (pct > 100)
		pct = 100;

	return (int)pct;
}

static int
read_wifi(int *quality, int *level)
{
	char buf[16384];
	char *save = NULL;
	char *line;

	if (!wlan_if[0])
		return -1;
	if (read_file("/proc/net/wireless", buf, sizeof buf) < 0)
		return -1;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *colon, *p, *end;
		char *fields[3];
		int nf = 0;
		long q, l;

		colon = strchr(line, ':');
		if (!colon)
			continue;
		p = colon + 1;
		*colon = '\0';
		trim_ws(line);
		if (strcmp(line, wlan_if) != 0)
			continue;

		/* Split the remaining fields.  Quality and level are printed
		 * with a trailing dot, e.g. "70.  -40."; strtol() stops at
		 * the dot, and each field is already isolated here. */
		while (nf < 3 && *p) {
			while (*p == ' ' || *p == '\t')
				p++;
			if (!*p)
				break;
			fields[nf++] = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
			if (*p)
				*p++ = '\0';
		}
		if (nf < 3)
			continue;

		q = strtol(fields[1], &end, 10);
		if (end == fields[1])
			continue;
		l = strtol(fields[2], &end, 10);
		if (end == fields[2])
			continue;
		if (q < INT_MIN || q > INT_MAX || l < INT_MIN || l > INT_MAX)
			continue;

		*quality = (int)q;
		*level = (int)l;
		return 0;
	}

	return -1;
}

static const char *
wifi_signal_symbol(int quality, int level)
{
	if (level < 0 && level > -120) {
		if (level >= -50)
			return SYM_WIFI_4;
		if (level >= -60)
			return SYM_WIFI_3;
		if (level >= -70)
			return SYM_WIFI_2;
		return SYM_WIFI_1;
	}

	if (quality >= 76)
		return SYM_WIFI_4;
	if (quality >= 51)
		return SYM_WIFI_3;
	if (quality >= 26)
		return SYM_WIFI_2;
	if (quality > 0)
		return SYM_WIFI_1;

	return NULL;
}

static const char *
get_wifi_symbol(void)
{
	const char *sym;
	int quality, level;

	if (!wlan_if[0])
		return SYM_WIFI_OFF;

	if (read_wifi(&quality, &level) == 0) {
		wifi_failures = 0;
		sym = wifi_signal_symbol(quality, level);
		if (sym)
			return sym;
	}

	/* Stats are missing or all zero.  sysfs can still tell us that the
	 * interface exists and is associated, which is the normal case for
	 * cfg80211 drivers without WEXT.  Show a connected icon rather than
	 * the off symbol when the signal level is unknown. */
	if (wifi_iface_up(wlan_if)) {
		wifi_failures = 0;
		return SYM_WIFI_4;
	}

	/* An interface that exists but is down should be sampled again soon,
	 * not treated as a vanished device.  This also lets it recover when
	 * it associates later. */
	if (wifi_iface_exists(wlan_if)) {
		wifi_failures = 0;
		return SYM_WIFI_OFF;
	}

	if (++wifi_failures >= 10) {
		/* Stop sampling an interface that is no longer there; the
		 * scheduler will retry discovery at WIFI_RETRY_INTERVAL
		 * instead of waking for a vanished device. */
		wlan_if[0] = '\0';
		wifi_failures = 0;
		wifi_next = now_ms() + interval_ms(WIFI_RETRY_INTERVAL);
	}

	return SYM_WIFI_OFF;
}

/* -------------------------------------------------------------------------
 * external helpers
 * ------------------------------------------------------------------------- */

static int
parse_volume(const char *line, int *muted)
{
	const char *p;
	int whole = 0;
	int frac = 0;
	int scale = 1;

	*muted = strstr(line, "[MUTED]") != NULL;

	p = strstr(line, "Volume:");
	if (!p)
		return -1;
	p += 7;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!isdigit((unsigned char)*p))
		return -1;

	while (isdigit((unsigned char)*p)) {
		if (whole < 100000)
			whole = whole * 10 + (*p - '0');
		p++;
	}

	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			if (scale < 100) {
				frac = frac * 10 + (*p - '0');
				scale *= 10;
			}
			p++;
		}
	}

	return whole * 100 + frac * 100 / scale;
}

static int
get_audio(const char *node, int *muted)
{
	char out[256];
	char *argv[] = { wpctl_path, "get-volume", (char *)node, NULL };

	*muted = 0;
	if (!have_wpctl)
		return -1;
	if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) < 0)
		return -1;

	return parse_volume(out, muted);
}

#ifdef HAVE_PULSE
static void pulse_server_info_cb(pa_context *c, const pa_server_info *i,
    void *userdata);
static void pulse_sink_info_cb(pa_context *c, const pa_sink_info *i, int eol,
    void *userdata);
static void pulse_source_info_cb(pa_context *c, const pa_source_info *i,
    int eol, void *userdata);

static void
pulse_check_ready(void)
{
	if (atomic_load(&pulse_server_ready) &&
	    atomic_load(&pulse_sink_ready) &&
	    atomic_load(&pulse_source_ready))
		atomic_store(&pulse_ready, 1);
}

static void
pulse_query_server(void)
{
	pa_operation *op;

	if (!pulse_ctx)
		return;
	op = pa_context_get_server_info(pulse_ctx, pulse_server_info_cb, NULL);
	if (op)
		pa_operation_unref(op);
}

static void
pulse_query_sink(void)
{
	pa_operation *op;

	if (!pulse_sink_name[0]) {
		atomic_store(&pulse_sink_pct, -1);
		return;
	}
	op = pa_context_get_sink_info_by_name(pulse_ctx, pulse_sink_name,
	    pulse_sink_info_cb, NULL);
	if (op)
		pa_operation_unref(op);
}

static void
pulse_query_source(void)
{
	pa_operation *op;

	if (!pulse_source_name[0]) {
		atomic_store(&pulse_source_pct, -1);
		return;
	}
	op = pa_context_get_source_info_by_name(pulse_ctx, pulse_source_name,
	    pulse_source_info_cb, NULL);
	if (op)
		pa_operation_unref(op);
}

static void
pulse_server_info_cb(pa_context *c, const pa_server_info *i, void *userdata)
{
	(void)c;
	(void)userdata;

	if (!i) {
		atomic_store(&pulse_server_ready, 1);
		atomic_store(&pulse_sink_ready, 1);
		atomic_store(&pulse_source_ready, 1);
		atomic_store(&pulse_ready, 1);
		wake_main();
		return;
	}

	copy_str(pulse_sink_name, sizeof pulse_sink_name,
	    i->default_sink_name ? i->default_sink_name : "");
	copy_str(pulse_source_name, sizeof pulse_source_name,
	    i->default_source_name ? i->default_source_name : "");
	if (!pulse_sink_name[0])
		atomic_store(&pulse_sink_ready, 1);
	if (!pulse_source_name[0])
		atomic_store(&pulse_source_ready, 1);
	pulse_query_sink();
	pulse_query_source();
	atomic_store(&pulse_server_ready, 1);
	pulse_check_ready();
	wake_main();
}

static void
pulse_sink_info_cb(pa_context *c, const pa_sink_info *i, int eol,
    void *userdata)
{
	pa_volume_t avg;

	(void)c;
	(void)userdata;

	if (eol > 0) {
		atomic_store(&pulse_sink_ready, 1);
		pulse_check_ready();
		return;
	}
	if (eol < 0 || !i) {
		int changed = atomic_exchange(&pulse_sink_pct, -1) != -1;

		atomic_store(&pulse_sink_ready, 1);
		pulse_check_ready();
		if (changed)
			wake_main();
		return;
	}

	avg = pa_cvolume_avg(&i->volume);
	{
		int pct = (int)(((uint64_t)avg * 100 + PA_VOLUME_NORM / 2) /
		    PA_VOLUME_NORM);
		int muted = i->mute ? 1 : 0;
		int changed = atomic_load(&pulse_sink_pct) != pct ||
		    atomic_load(&pulse_sink_muted) != muted;

		atomic_store(&pulse_sink_pct, pct);
		atomic_store(&pulse_sink_muted, muted);
		atomic_store(&pulse_sink_ready, 1);
		pulse_check_ready();
		if (changed)
			wake_main();
	}
}

static void
pulse_source_info_cb(pa_context *c, const pa_source_info *i, int eol,
    void *userdata)
{
	pa_volume_t avg;

	(void)c;
	(void)userdata;

	if (eol > 0) {
		atomic_store(&pulse_source_ready, 1);
		pulse_check_ready();
		return;
	}
	if (eol < 0 || !i) {
		int changed = atomic_exchange(&pulse_source_pct, -1) != -1;

		atomic_store(&pulse_source_ready, 1);
		pulse_check_ready();
		if (changed)
			wake_main();
		return;
	}

	avg = pa_cvolume_avg(&i->volume);
	{
		int pct = (int)(((uint64_t)avg * 100 + PA_VOLUME_NORM / 2) /
		    PA_VOLUME_NORM);
		int muted = i->mute ? 1 : 0;
		int changed = atomic_load(&pulse_source_pct) != pct ||
		    atomic_load(&pulse_source_muted) != muted;

		atomic_store(&pulse_source_pct, pct);
		atomic_store(&pulse_source_muted, muted);
		atomic_store(&pulse_source_ready, 1);
		pulse_check_ready();
		if (changed)
			wake_main();
	}
}

static void
pulse_subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
    uint32_t idx, void *userdata)
{
	pa_subscription_event_type_t facility;

	(void)c;
	(void)idx;
	(void)userdata;

	facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

	/* A volume or mute change is a sink/source event.  Re-querying the
	 * server would cost two extra round-trips and wake the status for the
	 * unchanged source as well, which is pure overhead while a volume key
	 * repeats.  The server is still queried when the default device or a
	 * device list changes; that is the only case where the names move. */
	switch (facility) {
	case PA_SUBSCRIPTION_EVENT_SERVER:
		pulse_query_server();
		break;
	case PA_SUBSCRIPTION_EVENT_SINK:
		pulse_query_sink();
		break;
	case PA_SUBSCRIPTION_EVENT_SOURCE:
		pulse_query_source();
		break;
	default:
		break;
	}
}

static void
pulse_state_cb(pa_context *c, void *userdata)
{
	pa_operation *op;

	(void)userdata;

	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		pa_context_set_subscribe_callback(c, pulse_subscribe_cb, NULL);
		op = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SERVER |
		    PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE,
		    NULL, NULL);
		if (op)
			pa_operation_unref(op);
		pulse_query_server();
		wake_main();
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		atomic_store(&pulse_dead, 1);
		if (pulse_ml)
			pa_mainloop_quit(pulse_ml, 1);
		wake_main();
		break;
	default:
		break;
	}
}

static void *
pulse_thread(void *arg)
{
	int ret = 0;

	(void)arg;

	pulse_ml = pa_mainloop_new();
	if (!pulse_ml) {
		atomic_store(&pulse_dead, 1);
		return NULL;
	}

	pulse_ctx = pa_context_new(pa_mainloop_get_api(pulse_ml),
	    "pluto-status");
	if (!pulse_ctx) {
		atomic_store(&pulse_dead, 1);
		pa_mainloop_free(pulse_ml);
		pulse_ml = NULL;
		return NULL;
	}

	pa_context_set_state_callback(pulse_ctx, pulse_state_cb, NULL);
	if (pa_context_connect(pulse_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
		atomic_store(&pulse_dead, 1);
		pa_context_unref(pulse_ctx);
		pa_mainloop_free(pulse_ml);
		pulse_ctx = NULL;
		pulse_ml = NULL;
		return NULL;
	}

	pa_mainloop_run(pulse_ml, &ret);

	pa_context_disconnect(pulse_ctx);
	pa_context_unref(pulse_ctx);
	pa_mainloop_free(pulse_ml);
	pulse_ctx = NULL;
	pulse_ml = NULL;
	atomic_store(&pulse_dead, 1);

	return NULL;
}

static void
start_pulse(void)
{
	pthread_t thread;
	int i;

	if (!(ENABLE_VOL || ENABLE_MIC))
		return;

	pulse_started = 1;
	if (pthread_create(&thread, NULL, pulse_thread, NULL) != 0) {
		pulse_started = 0;
		atomic_store(&pulse_dead, 1);
		return;
	}
	pthread_detach(thread);

	for (i = 0; i < 50; i++) {
		if (atomic_load(&pulse_ready) || atomic_load(&pulse_dead))
			break;
		sleep_ms(10);
	}
}
#endif

static int
get_audio_cached(int source, int *muted)
{
#ifdef HAVE_PULSE
	if (pulse_started && !atomic_load(&pulse_dead)) {
		int pct;

		if (!atomic_load(&pulse_ready)) {
			return get_audio(source ? "@DEFAULT_AUDIO_SOURCE@" :
			    "@DEFAULT_AUDIO_SINK@", muted);
		}

		pct = atomic_load(source ? &pulse_source_pct :
		    &pulse_sink_pct);
		if (pct < 0)
			return -1;
		*muted = atomic_load(source ? &pulse_source_muted :
		    &pulse_sink_muted);
		return pct;
	}
#endif
	return get_audio(source ? "@DEFAULT_AUDIO_SOURCE@" :
	    "@DEFAULT_AUDIO_SINK@", muted);
}

static int
audio_event_driven(void)
{
#ifdef HAVE_PULSE
	if (pulse_started && !atomic_load(&pulse_dead) &&
	    atomic_load(&pulse_ready))
		return 1;
#endif
	return 0;
}

static int
audio_available(void)
{
	return audio_event_driven() || have_wpctl;
}

static int
audio_polling(void)
{
	return !audio_event_driven() && have_wpctl;
}

static void
sample_audio(long long now)
{
	int polling;

	if (!(ENABLE_VOL || ENABLE_MIC))
		return;
	if (!audio_available()) {
		sink_valid = 0;
		source_valid = 0;
		return;
	}

	polling = audio_polling();

	/* With libpulse the callback already updated the cached value, so a
	 * wake must use it immediately.  Only the wpctl fallback polls, and
	 * only it needs the interval throttle. */
	if (polling) {
		if (now < audio_next)
			return;
		audio_next = now + interval_ms(AUDIO_INTERVAL);
	}

	if (ENABLE_VOL) {
		sink_value = get_audio_cached(0, &sink_muted);
		sink_valid = sink_value >= 0;
	}
	if (ENABLE_MIC) {
		source_value = get_audio_cached(1, &source_muted);
		source_valid = source_value >= 0;
	}
}

static void
clean_song(char *song)
{
	char *p;

	if (strcmp(song, " - ") == 0)
		song[0] = '\0';

	/* A newline or tab in a title would break the one-line protocol. */
	for (p = song; *p; p++)
		if ((unsigned char)*p < 0x20)
			*p = ' ';

	utf8_truncate(song, (size_t)MAX_SONG);
}

static void
get_music(char *state, size_t statesz, char *song, size_t songsz)
{
	char out[1024];
	char *sep;
	char *argv[] = {
		playerctl_path, "metadata", "--format",
		"{{status}}|{{artist}} - {{title}}", NULL
	};

	state[0] = '\0';
	song[0] = '\0';
	if (!have_playerctl)
		return;
	if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) < 0)
		return;

	chomp(out);
	sep = strchr(out, '|');
	if (!sep)
		return;
	*sep = '\0';

	copy_str(state, statesz, out);
	copy_str(song, songsz, sep + 1);
	clean_song(song);
}

static int
music_cached(void)
{
	int cached;

	pthread_mutex_lock(&music_lock);
	cached = music_state[0] != '\0';
	pthread_mutex_unlock(&music_lock);

	return cached;
}

static void
music_set(const char *state, const char *song)
{
	pthread_mutex_lock(&music_lock);
	copy_str(music_state, sizeof music_state, state);
	copy_str(music_song, sizeof music_song, song);
	pthread_mutex_unlock(&music_lock);
	wake_main();
}

static void
get_music_cached(char *state, size_t statesz, char *song, size_t songsz)
{
	if (!music_follow) {
		get_music(state, statesz, song, songsz);
		return;
	}

	/* playerctl --follow stays alive when the last player exits.  Only
	 * verify while something is cached, so a machine with no player does
	 * not pay for a periodic helper process. */
	if (now_ms() >= music_next_check) {
		int cached;

		pthread_mutex_lock(&music_lock);
		cached = music_state[0] != '\0';
		pthread_mutex_unlock(&music_lock);
		if (cached) {
			char out[256];
			char *argv[] = { playerctl_path, "--list-all", NULL };

			music_next_check = now_ms() + MUSIC_CHECK_MS;
			if (run_cmd(argv, out, sizeof out,
			    COMMAND_TIMEOUT_MS) == 0) {
				trim_ws(out);
				if (!out[0])
					music_set("", "");
			}
		}
	}

	pthread_mutex_lock(&music_lock);
	copy_str(state, statesz, music_state);
	copy_str(song, songsz, music_song);
	pthread_mutex_unlock(&music_lock);
}

static int
spawn_player_follow(pid_t *pid, int *fd)
{
	int fds[2];
	int rc;
	posix_spawn_file_actions_t fa;
	char *argv[] = {
		playerctl_path, "-F", "-f",
		"{{status}}|{{artist}} - {{title}}", "metadata", NULL
	};

	if (pipe(fds) < 0)
		return -1;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	if (fds[1] != STDOUT_FILENO) {
		posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&fa, fds[1]);
	}
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
	    O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
	    O_WRONLY, 0);

	rc = posix_spawnp(pid, playerctl_path, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0) {
		close(fds[0]);
		return -1;
	}

	player_pid = (sig_atomic_t)*pid;
	*fd = fds[0];
	return 0;
}

static void *
player_follow_loop(void *arg)
{
	char line[1024];

	(void)arg;

	for (;;) {
		pid_t pid;
		int fd;
		int abort_child = 0;
		size_t len = 0;

		if (spawn_player_follow(&pid, &fd) < 0) {
			sleep_ms(5000);
			continue;
		}

		for (;;) {
			char buf[512];
			ssize_t n;
			size_t i;

			n = read(fd, buf, sizeof buf);
			if (n > 0) {
				for (i = 0; i < (size_t)n; i++) {
					if (buf[i] == '\n') {
						char *sep;

						line[len] = '\0';
						chomp(line);
						if (!line[0]) {
							/* playerctl emits a blank
							 * line when the last player
							 * disappears. */
							music_set("", "");
						} else {
							sep = strchr(line, '|');
							if (sep) {
								char state[32];
								char song[512];

								*sep = '\0';
								copy_str(state,
								    sizeof state,
								    line);
								copy_str(song,
								    sizeof song,
								    sep + 1);
								clean_song(song);
								music_set(state,
								    song);
							}
						}
						len = 0;
					} else if (len < sizeof line - 1) {
						line[len++] = buf[i];
					}
				}
			} else if (n == 0) {
				break;
			} else if (errno != EINTR) {
				abort_child = 1;
				break;
			}
		}

		close(fd);
		if (abort_child)
			kill(pid, SIGTERM);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
		player_pid = 0;
		music_set("", "");
		sleep_ms(5000);
	}

	return NULL;
}

static void
start_player_follow(void)
{
	pthread_t thread;

	if (!ENABLE_SONG || !have_playerctl)
		return;

	{
		char state[32];
		char song[512];

		get_music(state, sizeof state, song, sizeof song);
		music_set(state, song);
	}

	if (pthread_create(&thread, NULL, player_follow_loop, NULL) != 0)
		return;
	pthread_detach(thread);
	music_follow = 1;
}

static int
get_bt(int *on)
{
	char out[4096];
	char *argv[] = { bluetoothctl_path, "show", NULL };

	*on = 0;
	if (!have_bluetoothctl)
		return 0;

	if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) == 0)
		*on = strstr(out, "Powered: yes") != NULL;

	return 0;
}

static const char *
get_power_profile(void)
{
	char buf[64];
	const char *sym;

	if (read_file("/sys/firmware/acpi/platform_profile", buf, sizeof buf) >= 0) {
		trim_ws(buf);
		if (buf[0]) {
			sym = power_profile_symbol(buf);
			if (sym)
				return sym;
		}
	}

	if (!have_powerprofilesctl)
		return NULL;

	{
		char out[64];
		char *argv[] = { powerprofilesctl_path, "get", NULL };

		pp_value[0] = '\0';
		if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) == 0) {
			trim_ws(out);
			copy_str(pp_value, sizeof pp_value, out);
		}
	}

	return pp_value[0] ? power_profile_symbol(pp_value) : NULL;
}

/* -------------------------------------------------------------------------
 * formatting helpers
 * ------------------------------------------------------------------------- */

static const char *
battery_symbol(int pct)
{
	if (pct >= BAT_FULL_THRESHOLD)
		return SYM_BAT_FULL;
	if (pct >= BAT_3_4_THRESHOLD)
		return SYM_BAT_HIGH;
	if (pct >= BAT_HALF_THRESHOLD)
		return SYM_BAT_HALF;

	return SYM_BAT_LOW;
}

static const char *
volume_symbol(int muted, int pct)
{
	if (muted || pct == 0)
		return SYM_VOL_MUTE;
	if (pct < VOL_LOW_THRESHOLD)
		return SYM_VOL_LOW;

	return SYM_VOL_HIGH;
}

static const char *
music_symbol(const char *state)
{
	if (strcmp(state, "Playing") == 0)
		return SYM_MUSIC_PLAY;
	if (strcmp(state, "Paused") == 0)
		return SYM_MUSIC_PAUSE;

	return NULL;
}

static const char *
power_profile_symbol(const char *profile)
{
	if (strcmp(profile, "performance") == 0)
		return SYM_POWER_PERFORMANCE;
	if (strcmp(profile, "balanced") == 0)
		return SYM_POWER_BALANCED;
	if (strcmp(profile, "power-saver") == 0 ||
	    strcmp(profile, "low-power") == 0)
		return SYM_POWER_POWERSAVE;

	return NULL;
}

static size_t
utf8_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xe0) == 0xc0)
		return 2;
	if ((c & 0xf0) == 0xe0)
		return 3;
	if ((c & 0xf8) == 0xf0)
		return 4;

	return 1;
}

/*
 * Truncate to max UTF-8 characters without splitting a sequence.  The shell
 * version used `cut -c`, which can cut a multibyte character in half.
 */
static void
utf8_truncate(char *s, size_t max)
{
	size_t i = 0;
	size_t chars = 0;

	while (s[i] && chars < max) {
		size_t l = utf8_len((unsigned char)s[i]);
		size_t j;

		for (j = 1; j < l; j++) {
			if (!s[i + j] ||
			    ((unsigned char)s[i + j] & 0xc0) != 0x80) {
				l = 1;
				break;
			}
		}
		i += l;
		chars++;
	}

	if (s[i])
		s[i] = '\0';
}

static void
get_clock(char *buf, size_t size)
{
	time_t t;
	struct tm tm;

	buf[0] = '\0';
	t = time(NULL);
	if (t == (time_t)-1 || !localtime_r(&t, &tm))
		return;
	if (strftime(buf, size, CLOCKFMT, &tm) == 0)
		buf[0] = '\0';
}

static long long
next_clock_ms(long long now)
{
	struct timespec rt;
	struct tm tm;
	long long delay;
	int interval = CLOCK_INTERVAL > 0 ? CLOCK_INTERVAL : 1;
	int second;
	int rem;

	if (clock_gettime(CLOCK_REALTIME, &rt) < 0 ||
	    !localtime_r(&rt.tv_sec, &tm))
		return now + interval_ms(interval);

	second = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
	rem = second % interval;
	delay = (long long)(interval - rem) * 1000 -
	    rt.tv_nsec / 1000000L;
	if (delay <= 0)
		delay += interval_ms(interval);

	return now_ms() + delay;
}

/* -------------------------------------------------------------------------
 * status line
 * ------------------------------------------------------------------------- */

static void
build_status(struct outbuf *o, long long now)
{
	const char *sym;

	o->len = 0;
	o->s[0] = '\0';

	if (ENABLE_CPU) {
		if (module_due(&cpu_next, CPU_INTERVAL, now)) {
			cpu_value = get_cpu();
			cpu_valid = cpu_value >= 0;
		}
		if (cpu_valid)
			out_module(o, SEPARATE_CPU, "%s %d%%", SYM_CPU,
			    cpu_value);
	}

	if (ENABLE_RAM) {
		if (module_due(&ram_next, RAM_INTERVAL, now)) {
			ram_value = get_ram();
			ram_valid = ram_value >= 0;
		}
		if (ram_valid)
			out_module(o, SEPARATE_RAM, "%s %d%%", SYM_RAM,
			    ram_value);
	}

	if (TOGGLE_HOME) {
		if (module_due(&home_next, HOME_INTERVAL, now))
			home_valid = get_home_gb(&home_value) == 0;
		if (home_valid)
			out_module(o, SEPARATE_HOME, "%s %lluG", SYM_HOME,
			    home_value);
	}

	if (TOGGLE_UPTIME) {
		if (module_due(&uptime_next, UPTIME_INTERVAL, now))
			uptime_valid = get_uptime_hours(&uptime_value) == 0;
		if (uptime_valid)
			out_module(o, SEPARATE_UPTIME, "%s %lluh", SYM_UPTIME,
			    uptime_value);
	}

	if (ENABLE_WIFI) {
		if (wlan_if[0]) {
			if (module_due(&wifi_sample_next, WIFI_INTERVAL, now)) {
				wifi_symbol = get_wifi_symbol();
				wifi_valid = 1;
			}
		} else if (module_due(&wifi_next, WIFI_RETRY_INTERVAL, now)) {
			find_wifi();
			if (wlan_if[0]) {
				wifi_sample_next = now + interval_ms(WIFI_INTERVAL);
				wifi_symbol = get_wifi_symbol();
			} else {
				wifi_symbol = SYM_WIFI_OFF;
			}
			wifi_valid = 1;
		}
		if (wifi_valid)
			out_module(o, SEPARATE_WIFI, "%s", wifi_symbol);
	}

	if (ENABLE_BT) {
		if (have_bluetoothctl &&
		    module_due(&bt_next, BLUETOOTH_INTERVAL, now)) {
			get_bt(&bt_on);
			bt_valid = 1;
		}
		if (bt_valid) {
			if (bt_on)
				out_module(o, SEPARATE_BT, "%s", SYM_BT);
			else
				out_module(o, SEPARATE_BT, "%s Off",
				    SYM_BT_OFF);
		}
	}

	if (ENABLE_TEMP && !temp_missing) {
		if (module_due(&temp_next, TEMP_INTERVAL, now)) {
			if (get_temp(&temp_value) == 0) {
				temp_valid = 1;
			} else {
				temp_value = -1;
				temp_valid = 0;
			}
		}
		if (temp_valid)
			out_module(o, SEPARATE_TEMP, "%s %d°C", SYM_TEMP,
			    temp_value);
	}

	if (ENABLE_VOL || ENABLE_MIC)
		sample_audio(now);

	if (ENABLE_VOL && sink_valid) {
		sym = volume_symbol(sink_muted, sink_value);
		if (sink_muted || sink_value == 0)
			out_module(o, SEPARATE_VOLUME, "%s Mute", sym);
		else
			out_module(o, SEPARATE_VOLUME, "%s %d%%", sym,
			    sink_value);
	}

	if (ENABLE_MIC && source_valid) {
		if (source_muted)
			out_module(o, SEPARATE_MIC, "%s Mute", SYM_MIC_OFF);
		else
			out_module(o, SEPARATE_MIC, "%s %d%%", SYM_MIC_ON,
			    source_value);
	}

	if (ENABLE_BAT && !battery_missing) {
		if (module_due(&battery_next, BATTERY_INTERVAL, now))
			battery_valid = get_battery(&battery_pct,
			    &battery_charging) == 0;
		if (battery_valid) {
			if (battery_charging)
				out_module(o, SEPARATE_BATTERY, "%s %d%%",
				    SYM_CHARGING, battery_pct);
			else
				out_module(o, SEPARATE_BATTERY, "%s %d%%",
				    battery_symbol(battery_pct), battery_pct);
		}
	}

	if (ENABLE_POWER_PROFILE && !power_missing) {
		if (module_due(&power_next, POWER_PROFILE_INTERVAL, now)) {
			power_symbol = get_power_profile();
			power_valid = power_symbol != NULL;
		}
		if (power_valid)
			out_module(o, SEPARATE_POWER_PROFILE, "%s",
			    power_symbol);
	}

	if (TOGGLE_BRIGHTNESS && !brightness_missing) {
		if (module_due(&brightness_next, BRIGHTNESS_INTERVAL, now)) {
			brightness_value = get_brightness();
			brightness_valid = brightness_value >= 0;
		}
		if (brightness_valid)
			out_module(o, SEPARATE_BRIGHTNESS, "%s %d%%",
			    SYM_BRIGHTNESS, brightness_value);
	}

	if (ENABLE_SONG && have_playerctl) {
		char state[32];
		char song[512];

		if (!music_follow) {
			if (now >= music_poll_next) {
				get_music_cached(music_poll_state,
				    sizeof music_poll_state, music_poll_song,
				    sizeof music_poll_song);
				music_poll_next = now + MUSIC_CHECK_MS;
			}
			copy_str(state, sizeof state, music_poll_state);
			copy_str(song, sizeof song, music_poll_song);
		} else {
			get_music_cached(state, sizeof state, song, sizeof song);
		}

		sym = music_symbol(state);
		if (sym) {
			if (song[0])
				out_module(o, SEPARATE_MUSIC, "%s %s", sym,
				    song);
			else
				out_module(o, SEPARATE_MUSIC, "%s", sym);
		}
	}

	if (ENABLE_CLOCK) {
		if (module_due(&clock_next, CLOCK_INTERVAL, now)) {
			get_clock(clock_value, sizeof clock_value);
			clock_valid = clock_value[0] != '\0';
			clock_next = next_clock_ms(now);
		}
		if (clock_valid)
			out_module(o, SEPARATE_CLOCK, "%s", clock_value);
	}
}

static long long
next_due_ms(void)
{
	long long next = LLONG_MAX;

	if (ENABLE_CPU && cpu_next < next)
		next = cpu_next;
	if (ENABLE_RAM && ram_next < next)
		next = ram_next;
	if (TOGGLE_HOME && home_next < next)
		next = home_next;
	if (TOGGLE_UPTIME && uptime_next < next)
		next = uptime_next;
	if (ENABLE_WIFI) {
		if (wlan_if[0]) {
			if (wifi_sample_next < next)
				next = wifi_sample_next;
		} else if (wifi_next < next) {
			next = wifi_next;
		}
	}
	if (ENABLE_BT && have_bluetoothctl && bt_next < next)
		next = bt_next;
	if (ENABLE_TEMP && !temp_missing && temp_next < next)
		next = temp_next;
	if (ENABLE_BAT && !battery_missing && battery_next < next)
		next = battery_next;
	if (ENABLE_POWER_PROFILE && !power_missing && power_next < next)
		next = power_next;
	if (TOGGLE_BRIGHTNESS && !brightness_missing &&
	    brightness_next < next)
		next = brightness_next;
	if ((ENABLE_VOL || ENABLE_MIC) && audio_polling() &&
	    audio_next < next)
		next = audio_next;
	if (ENABLE_CLOCK && clock_next < next)
		next = clock_next;

	if (ENABLE_SONG && have_playerctl) {
		if (music_follow) {
			if (music_cached() && music_next_check < next)
				next = music_next_check;
		} else if (music_poll_next < next) {
			next = music_poll_next;
		}
	}

	if (next == LLONG_MAX)
		next = now_ms() + 60000;

	return next;
}

/* -------------------------------------------------------------------------
 * setup / main
 * ------------------------------------------------------------------------- */

static void
init_state(void)
{
	have_playerctl = find_cmd_path("playerctl", playerctl_path,
	    sizeof playerctl_path) == 0;
	have_wpctl = find_cmd_path("wpctl", wpctl_path,
	    sizeof wpctl_path) == 0;
	have_powerprofilesctl = find_cmd_path("powerprofilesctl",
	    powerprofilesctl_path, sizeof powerprofilesctl_path) == 0;
	have_brightnessctl = find_cmd_path("brightnessctl",
	    brightnessctl_path, sizeof brightnessctl_path) == 0;
	have_bluetoothctl = find_cmd_path("bluetoothctl",
	    bluetoothctl_path, sizeof bluetoothctl_path) == 0;

	if (ENABLE_BAT) {
		find_battery();
		battery_missing = !battery_dir[0];
	}
	if (ENABLE_TEMP) {
		find_temp();
		temp_missing = !temp_path[0];
	}
	if (ENABLE_WIFI) {
		find_wifi();
		if (!wlan_if[0]) {
			/* Keep the same "off" field as before, but only retry
			 * discovery occasionally instead of sampling every
			 * WIFI_INTERVAL. */
			wifi_symbol = SYM_WIFI_OFF;
			wifi_valid = 1;
			wifi_next = now_ms() + interval_ms(WIFI_RETRY_INTERVAL);
		}
	}

	if (TOGGLE_BRIGHTNESS) {
		find_backlight();
		if (!backlight_path[0] && have_brightnessctl) {
			char out[64];
			char *argv[] = { brightnessctl_path, "max", NULL };

			if (run_cmd(argv, out, sizeof out, COMMAND_TIMEOUT_MS) == 0)
				backlight_max = parse_int(out);
			if (backlight_max <= 0)
				backlight_max = -1;
		}
		brightness_missing = !backlight_path[0] &&
		    (!have_brightnessctl || backlight_max <= 0);
	}

	if (ENABLE_POWER_PROFILE)
		power_missing = !have_powerprofilesctl &&
		    access("/sys/firmware/acpi/platform_profile", R_OK) != 0;

	if (ENABLE_BT && !have_bluetoothctl) {
		bt_on = 0;
		bt_valid = 1;
	}

#ifndef HAVE_PULSE
	if ((ENABLE_VOL || ENABLE_MIC) && !have_wpctl)
		fprintf(stderr, "pluto-status: wpctl not found; "
		    "volume and microphone are disabled\n");
#endif
	if (ENABLE_SONG && !have_playerctl)
		fprintf(stderr, "pluto-status: playerctl not found; "
		    "song is disabled\n");
	if (ENABLE_BT && !have_bluetoothctl)
		fprintf(stderr, "pluto-status: bluetoothctl not found; "
		    "bluetooth will show as off\n");
	if (ENABLE_POWER_PROFILE && !have_powerprofilesctl &&
	    access("/sys/firmware/acpi/platform_profile", R_OK) != 0)
		fprintf(stderr, "pluto-status: powerprofilesctl not found; "
		    "power profile is disabled\n");
	if (TOGGLE_BRIGHTNESS && !backlight_path[0] && !have_brightnessctl)
		fprintf(stderr, "pluto-status: no backlight device; "
		    "brightness is disabled\n");
}

static void
usage(FILE *f, const char *prog)
{
	fprintf(f, "usage: %s [--once] [--help] [--version]\n"
	    "  --once     print one status line and exit\n", prog);
}

int
main(int argc, char *argv[])
{
	struct outbuf o;
	long long next;
	int once = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--once") == 0)
			once = 1;
		else if (strcmp(argv[i], "--help") == 0) {
			usage(stdout, argv[0]);
			return 0;
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("pluto-status " VERSION "\n");
			return 0;
		} else {
			usage(stderr, argv[0]);
			return 2;
		}
	}

	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, term_handler);
	signal(SIGTERM, term_handler);
	atexit(cleanup_player);

	init_state();
	init_wake_pipe();

#ifdef HAVE_PULSE
	start_pulse();
#endif

	if (once) {
		build_status(&o, now_ms());
		if (emit(&o) < 0)
			return errno == EPIPE ? 0 : 1;
		return 0;
	}

	start_player_follow();

	for (;;) {
		static char previous[OUTSIZE];
		static size_t previous_len;
		static int have_previous;

		build_status(&o, now_ms());
		if (!have_previous || o.len != previous_len ||
		    memcmp(o.s, previous, o.len) != 0) {
			if (emit(&o) < 0)
				return errno == EPIPE ? 0 : 1;
			memcpy(previous, o.s, o.len);
			previous_len = o.len;
			have_previous = 1;
		}

		next = next_due_ms();
		wait_for_wake(next);
	}
}
