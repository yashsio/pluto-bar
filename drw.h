/* See LICENSE file for copyright and license details. */
#ifndef DRW_H
#define DRW_H

#include <stddef.h>
#include <stdint.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

enum { ColFg, ColBg, ColLast };

typedef uint32_t Clr; /* 0xAARRGGBB */

typedef struct {
	cairo_surface_t *surf;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *font;
	PangoFontMetrics *metrics;
	int w, h;      /* logical size */
	int scale;
	int fonth;     /* ascent + descent in pixels */
	int ascent;
	int lrpad;     /* dwm's left/right padding = font height */
	Clr *scheme;
} Drw;

Drw *drw_create(int w, int h, int scale, void *data, int stride, const char *fontname);
void drw_free(Drw *drw);
void drw_setscheme(Drw *drw, Clr *scheme);
void drw_rect(Drw *drw, int x, int y, int w, int h, int filled, int invert);
int drw_text(Drw *drw, int x, int y, int w, int h, int lpad, const char *text, int invert);
int drw_text_width(Drw *drw, const char *text);
Clr *drw_scm_create(const char *clrnames[], size_t clrcount);
void drw_scm_free(Clr *scheme);
void drw_font_metrics(const char *fontname, int *height, int *ascent);

#endif
