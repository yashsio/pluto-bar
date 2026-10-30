/* See LICENSE file for copyright and license details. */
#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

#include "bar.h"

/* One implementation per compositor.  Adding a compositor means adding a
 * file like backend-foo.c, one entry in backend.c, and one object in the
 * Makefile.  Removing it is the same in reverse.
 *
 * Backends only touch MonitorState and the bar's small helper API.  They do
 * not draw, own Wayland objects, or include config.h. */
struct Backend {
	const char *name;

	/* Return 1 when this compositor is present.  May record state that
	 * connect() and the other callbacks need.  Called after the initial
	 * registry roundtrips. */
	int (*detect)(void);

	/* Observe wl_registry globals before backend selection.  Optional. */
	void (*global)(const char *interface, uint32_t version);

	/* Open the compositor socket.  Return 0 on success, -1 on failure. */
	int (*connect)(void);

	/* fd to poll for events, or -1 when disconnected. */
	int (*fd)(void);

	/* Read and apply available events. */
	void (*read_events)(void);

	/* Re-read all monitor state after a connect or a coalesced event. */
	void (*sync)(void);

	/* Switch a monitor to the given 0-based tag. */
	void (*tag_click)(Monitor *m, int tag);

	/* Periodic reconnect/liveness work. */
	void (*tick)(void);

	/* True while the backend is waiting for a reconnect retry. */
	int (*needs_retry)(void);

	/* Close sockets and release backend state. */
	void (*destroy)(void);
};

extern const struct Backend *backend;

void backend_select(void);
void backend_global(const char *interface, uint32_t version);
int backend_fd(void);

#endif
