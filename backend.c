/* See LICENSE file for copyright and license details. */
#include <stddef.h>

#include "backend.h"

/* The order is the detection priority.  Add or remove entries here. */
extern const struct Backend backend_hyprland;
extern const struct Backend backend_river;

static const struct Backend *const backends[] = {
	&backend_hyprland,
	&backend_river,
};

const struct Backend *backend;

void
backend_select(void)
{
	size_t i;

	for (i = 0; i < sizeof backends / sizeof backends[0]; i++) {
		if (backends[i]->detect && backends[i]->detect()) {
			backend = backends[i];
			return;
		}
	}
	backend = NULL;
}

void
backend_global(const char *interface, uint32_t version)
{
	size_t i;

	for (i = 0; i < sizeof backends / sizeof backends[0]; i++)
		if (backends[i]->global)
			backends[i]->global(interface, version);
}

int
backend_fd(void)
{
	if (!backend || !backend->fd)
		return -1;
	return backend->fd();
}
