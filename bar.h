/* See LICENSE file for copyright and license details. */
#ifndef BAR_H
#define BAR_H

#include <stddef.h>
#include <stdint.h>

#include "drw.h"

/* Keep this independent of config.h: compositor backends include bar.h but
 * must not inherit the bar's appearance configuration. */
#define MAX_TAGS 32

struct wl_output;
struct wl_surface;
struct wl_buffer;
struct zwlr_layer_surface_v1;

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
	int tagw[MAX_TAGS];
	int configured;
	unsigned int tagset;      /* bit i = tag i is selected */
	unsigned int occ;         /* bit i = tag i has clients */
	int focused;
	int floating;
	char title[512];
	char ltsym[16];           /* backend layout symbol, else config ltsymbol */
	Monitor *next;
};

/* A compositor backend fills this in; the bar applies it to a Monitor. */
struct MonitorState {
	unsigned int tagset;
	unsigned int occ;
	const char *layout;       /* NULL or "-" means config ltsymbol */
	int focused;
	int floating;
	const char *title;
};

/* core services available to backends */
Monitor *bar_monitors(void);
Monitor *bar_find_monitor(const char *name);
size_t bar_tag_count(void);
void bar_reset_monitor(Monitor *m);
void bar_reset_monitors(void);
void bar_apply_monitor(Monitor *m, const struct MonitorState *state);
void bar_redraw(void);
void bar_request_sync(void);
int bar_take_sync_request(void);
void bar_timer_update(void);

#endif
