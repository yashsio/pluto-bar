/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drw.h"

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static Clr
clr_create(const char *name)
{
	const char *s;
	size_t len;
	unsigned int a = 0xff, r, g, b;
	int v[8];
	size_t i;

	if (!name || name[0] != '#')
		die("pluto-bar: colour must be #rgb, #rrggbb or #aarrggbb: %s\n",
		    name ? name : "(null)");

	s = name + 1;
	len = strlen(s);
	if (len != 3 && len != 6 && len != 8)
		die("pluto-bar: invalid colour '%s'\n", name);

	for (i = 0; i < len; i++)
		if ((v[i] = hexval(s[i])) < 0)
			die("pluto-bar: invalid colour '%s'\n", name);

	if (len == 3) {
		r = v[0] * 17;
		g = v[1] * 17;
		b = v[2] * 17;
	} else if (len == 6) {
		r = v[0] * 16 + v[1];
		g = v[2] * 16 + v[3];
		b = v[4] * 16 + v[5];
	} else {
		a = v[0] * 16 + v[1];
		r = v[2] * 16 + v[3];
		g = v[4] * 16 + v[5];
		b = v[6] * 16 + v[7];
	}

	return (a << 24) | (r << 16) | (g << 8) | b;
}

static void
setclr(Drw *drw, Clr c)
{
	cairo_set_source_rgba(drw->cr,
	    ((c >> 16) & 0xff) / 255.0,
	    ((c >> 8) & 0xff) / 255.0,
	    (c & 0xff) / 255.0,
	    ((c >> 24) & 0xff) / 255.0);
}

static PangoFontDescription *
fontdesc(const char *fontname)
{
	PangoFontDescription *desc;

	desc = pango_font_description_from_string(fontname);
	if (!desc || !pango_font_description_get_family(desc))
		die("pluto-bar: cannot parse font '%s'\n", fontname);

	return desc;
}

Clr *
drw_scm_create(const char *clrnames[], size_t clrcount)
{
	Clr *scheme;
	size_t i;

	scheme = calloc(clrcount, sizeof(Clr));
	if (!scheme)
		die("pluto-bar: out of memory\n");

	for (i = 0; i < clrcount; i++)
		scheme[i] = clr_create(clrnames[i]);

	return scheme;
}

void
drw_scm_free(Clr *scheme)
{
	free(scheme);
}

void
drw_font_metrics(const char *fontname, int *height, int *ascent)
{
	PangoFontDescription *desc;
	PangoFontMetrics *metrics;
	PangoContext *ctx;
	PangoFontMap *map;
	int a, d;

	map = pango_cairo_font_map_get_default();
	ctx = pango_font_map_create_context(map);
	desc = fontdesc(fontname);
	metrics = pango_context_get_metrics(ctx, desc, NULL);

	a = PANGO_PIXELS(pango_font_metrics_get_ascent(metrics));
	d = PANGO_PIXELS(pango_font_metrics_get_descent(metrics));

	if (height)
		*height = a + d;
	if (ascent)
		*ascent = a;

	pango_font_metrics_unref(metrics);
	pango_font_description_free(desc);
	g_object_unref(ctx);
}

Drw *
drw_create(int w, int h, int scale, void *data, int stride, const char *fontname)
{
	Drw *drw;
	int a, d;

	if (w <= 0 || h <= 0)
		return NULL;

	drw = calloc(1, sizeof(Drw));
	if (!drw)
		die("pluto-bar: out of memory\n");

	drw->w = w;
	drw->h = h;
	drw->scale = scale > 0 ? scale : 1;

	drw->surf = cairo_image_surface_create_for_data(data,
	    CAIRO_FORMAT_ARGB32,
	    w * drw->scale, h * drw->scale, stride);
	if (cairo_surface_status(drw->surf) != CAIRO_STATUS_SUCCESS)
		die("pluto-bar: cannot create cairo surface\n");
	cairo_surface_set_device_scale(drw->surf, drw->scale, drw->scale);

	drw->cr = cairo_create(drw->surf);
	drw->layout = pango_cairo_create_layout(drw->cr);
	drw->font = fontdesc(fontname);
	pango_layout_set_font_description(drw->layout, drw->font);

	drw->metrics = pango_context_get_metrics(
	    pango_layout_get_context(drw->layout), drw->font, NULL);
	a = PANGO_PIXELS(pango_font_metrics_get_ascent(drw->metrics));
	d = PANGO_PIXELS(pango_font_metrics_get_descent(drw->metrics));
	drw->fonth = a + d;
	drw->ascent = a;
	drw->lrpad = drw->fonth;

	return drw;
}

void
drw_free(Drw *drw)
{
	if (!drw)
		return;

	if (drw->metrics)
		pango_font_metrics_unref(drw->metrics);
	if (drw->font)
		pango_font_description_free(drw->font);
	if (drw->layout)
		g_object_unref(drw->layout);
	if (drw->cr)
		cairo_destroy(drw->cr);
	if (drw->surf)
		cairo_surface_destroy(drw->surf);
	free(drw);
}

void
drw_setscheme(Drw *drw, Clr *scheme)
{
	if (drw)
		drw->scheme = scheme;
}

void
drw_rect(Drw *drw, int x, int y, int w, int h, int filled, int invert)
{
	if (!drw || !drw->scheme || w <= 0 || h <= 0)
		return;

	cairo_save(drw->cr);
	cairo_set_operator(drw->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_antialias(drw->cr, CAIRO_ANTIALIAS_NONE);
	setclr(drw, drw->scheme[invert ? ColBg : ColFg]);
	if (filled) {
		cairo_rectangle(drw->cr, x, y, w, h);
		cairo_fill(drw->cr);
	} else {
		cairo_set_line_width(drw->cr, 1);
		cairo_rectangle(drw->cr, x + 0.5, y + 0.5, w - 1, h - 1);
		cairo_stroke(drw->cr);
	}
	cairo_restore(drw->cr);
}

int
drw_text(Drw *drw, int x, int y, int w, int h, int lpad, const char *text, int invert)
{
	int ox = x, ow = w, baseline;

	if (!drw || !drw->scheme)
		return 0;

	/* dwm fills each text cell with the scheme background first */
	cairo_save(drw->cr);
	cairo_set_operator(drw->cr, CAIRO_OPERATOR_SOURCE);
	setclr(drw, drw->scheme[invert ? ColFg : ColBg]);
	cairo_rectangle(drw->cr, x, y, w, h);
	cairo_fill(drw->cr);
	cairo_restore(drw->cr);

	if (w < lpad)
		return ox + ow;
	if (!text || !*text)
		return ox + ow;

	x += lpad;
	w -= lpad;
	if (w <= 0)
		return ox + ow;

	cairo_save(drw->cr);
	cairo_rectangle(drw->cr, x, y, w, h);
	cairo_clip(drw->cr);
	setclr(drw, drw->scheme[invert ? ColBg : ColFg]);

	pango_layout_set_width(drw->layout, w * PANGO_SCALE);
	pango_layout_set_ellipsize(drw->layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_single_paragraph_mode(drw->layout, TRUE);
	pango_layout_set_text(drw->layout, text, -1);

	baseline = pango_layout_get_baseline(drw->layout);
	cairo_move_to(drw->cr, x,
	    y + (h - drw->fonth) / 2 + drw->ascent
	    - baseline / (double)PANGO_SCALE);
	pango_cairo_show_layout(drw->cr, drw->layout);

	cairo_restore(drw->cr);

	return ox + ow;
}

int
drw_text_width(Drw *drw, const char *text)
{
	int w = 0, h = 0;

	if (!drw || !text)
		return 0;

	pango_layout_set_width(drw->layout, -1);
	pango_layout_set_ellipsize(drw->layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_single_paragraph_mode(drw->layout, TRUE);
	pango_layout_set_text(drw->layout, text, -1);
	pango_layout_get_pixel_size(drw->layout, &w, &h);

	return w;
}
