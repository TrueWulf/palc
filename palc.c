/* palc - wallpaper palette
 * Copyright (c) 2026 truewulf
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define _POSIX_C_SOURCE 200809L

#include <Imlib2.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "0"
#endif

#define DEFAULT_COLORS 0
#define MAX_COLORS 32
#define MAX_TERMINAL 16
#define MAX_CLUSTERS (MAX_COLORS - 2)
#define MAX_TEMPLATES 64
#define KMEANS_ITERS 12
#define SAMPLE_TARGET 65536

enum { MODE_TERM, MODE_JSON, MODE_PLAIN, MODE_OSC, MODE_RESET };

struct cluster {
	uint8_t r, g, b;
	uint64_t weight;
};

struct palette {
	uint8_t r[MAX_COLORS], g[MAX_COLORS], b[MAX_COLORS];
	uint64_t weight[MAX_COLORS];
	uint8_t bg[3], fg[3];
	size_t n;
};

static void die(const char *msg, const char *arg)
{
	if (arg)
		fprintf(stderr, "palc: %s: %s\n", msg, arg);
	else
		fprintf(stderr, "palc: %s\n", msg);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);

	if (!p)
		die("out of memory", NULL);
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p)
		die("out of memory", NULL);
	return p;
}

static uint64_t luma(uint8_t r, uint8_t g, uint8_t b)
{
	return 2126ull * r + 7152ull * g + 722ull * b;
}

static void load_samples(const char *path, uint32_t **out, size_t *count)
{
	Imlib_Image img;
	Imlib_Load_Error err;
	DATA32 *raw;
	size_t w, h, total, step, x, y, n;
	uint32_t *samples;

	err = IMLIB_LOAD_ERROR_NONE;
	img = imlib_load_image_with_error_return(path, &err);
	if (!img || err != IMLIB_LOAD_ERROR_NONE)
		die("cannot load image", path);
	imlib_context_set_image(img);

	w = imlib_image_get_width();
	h = imlib_image_get_height();
	total = w * h;
	if (!total)
		die("empty image", path);

	step = 1;
	while (total / (step * step) > SAMPLE_TARGET)
		step *= 2;

	n = ((w + step - 1) / step) * ((h + step - 1) / step);
	samples = xmalloc(n * sizeof(*samples));

	n = 0;
	raw = imlib_image_get_data_for_reading_only();
	for (y = 0; y < h; y += step) {
		for (x = 0; x < w; x += step) {
			DATA32 p = raw[y * w + x];
			uint32_t a = (p >> 24) & 0xff;
			uint32_t r = ((p >> 16) & 0xff) * a / 255;
			uint32_t g = ((p >> 8) & 0xff) * a / 255;
			uint32_t b = (p & 0xff) * a / 255;

			samples[n++] = (r << 16) | (g << 8) | b;
		}
	}
	imlib_free_image();

	*out = samples;
	*count = n;
}

static int seed_unique(const struct cluster *seeds, size_t nseeds, uint32_t c)
{
	size_t i;

	for (i = 0; i < nseeds; i++) {
		if (c == ((uint32_t)seeds[i].r << 16 |
			  (uint32_t)seeds[i].g << 8 | seeds[i].b))
			return 0;
	}
	return 1;
}

static size_t pick_seeds(const uint32_t *px, size_t n, size_t k,
			 struct cluster *seeds)
{
	size_t s = 0, i, stride = n / k + 1;

	for (i = 0; i < n && s < k; i += stride) {
		if (seed_unique(seeds, s, px[i])) {
			seeds[s].r = (px[i] >> 16) & 0xff;
			seeds[s].g = (px[i] >> 8) & 0xff;
			seeds[s].b = px[i] & 0xff;
			seeds[s].weight = 0;
			s++;
		}
	}
	for (i = 0; i < n && s < k; i++) {
		if (seed_unique(seeds, s, px[i])) {
			seeds[s].r = (px[i] >> 16) & 0xff;
			seeds[s].g = (px[i] >> 8) & 0xff;
			seeds[s].b = px[i] & 0xff;
			seeds[s].weight = 0;
			s++;
		}
	}
	return s;
}

static int cluster_cmp(const void *a, const void *b)
{
	const struct cluster *ca = a, *cb = b;

	if (ca->weight != cb->weight)
		return ca->weight < cb->weight ? 1 : -1;
	return luma(cb->r, cb->g, cb->b) < luma(ca->r, ca->g, ca->b) ? -1 : 1;
}

static size_t cluster_pixels(const uint32_t *px, size_t n, size_t k,
			     struct cluster *out)
{
	struct cluster seeds[MAX_CLUSTERS];
	uint64_t sr[MAX_CLUSTERS], sg[MAX_CLUSTERS], sb[MAX_CLUSTERS];
	uint64_t cnt[MAX_CLUSTERS];
	uint64_t total_weight = n;
	size_t nc, iter, i, j;

	if (k > n)
		k = n;
	nc = pick_seeds(px, n, k, seeds);
	if (!nc)
		die("no pixels to cluster", NULL);

	for (iter = 0; iter < KMEANS_ITERS; iter++) {
		int moved = 0;

		memset(sr, 0, nc * sizeof(*sr));
		memset(sg, 0, nc * sizeof(*sg));
		memset(sb, 0, nc * sizeof(*sb));
		memset(cnt, 0, nc * sizeof(*cnt));

		for (i = 0; i < n; i++) {
			int32_t r = (px[i] >> 16) & 0xff;
			int32_t g = (px[i] >> 8) & 0xff;
			int32_t b = px[i] & 0xff;
			uint64_t best = UINT64_MAX;
			size_t bi = 0;

			for (j = 0; j < nc; j++) {
				int32_t dr = r - seeds[j].r;
				int32_t dg = g - seeds[j].g;
				int32_t db = b - seeds[j].b;
				uint64_t d = (uint64_t)(dr * dr) +
					     (uint64_t)(dg * dg) +
					     (uint64_t)(db * db);

				if (d < best) {
					best = d;
					bi = j;
				}
			}
			sr[bi] += r;
			sg[bi] += g;
			sb[bi] += b;
			cnt[bi]++;
		}

		for (j = 0; j < nc; j++) {
			uint8_t r, g, b;

			if (!cnt[j])
				continue;
			r = sr[j] / cnt[j];
			g = sg[j] / cnt[j];
			b = sb[j] / cnt[j];
			if (r != seeds[j].r || g != seeds[j].g ||
			    b != seeds[j].b)
				moved = 1;
			seeds[j].r = r;
			seeds[j].g = g;
			seeds[j].b = b;
			seeds[j].weight = cnt[j];
		}

		if (!moved)
			break;
	}

	memcpy(out, seeds, nc * sizeof(*out));
	qsort(out, nc, sizeof(*out), cluster_cmp);
	j = 0;
	for (i = 0; i < nc; i++) {
		if (!out[i].weight || out[i].weight * 333 < total_weight)
			continue;
		if (j && out[i].r == out[j - 1].r &&
		    out[i].g == out[j - 1].g && out[i].b == out[j - 1].b) {
			out[j - 1].weight += out[i].weight;
			continue;
		}
		out[j] = out[i];
		j++;
	}
	return j;
}

static void build_palette(const struct cluster *cl, size_t ncl, size_t want,
			  struct palette *pal)
{
	struct cluster sorted[MAX_CLUSTERS];
	size_t dark = 0, light = 0, n = 0, i, j;
	uint64_t total = 0;

	pal->n = 0;
	if (!ncl)
		return;
	for (i = 0; i < ncl; i++)
		total += cl[i].weight;
	for (i = 1; i < ncl; i++) {
		if (luma(cl[i].r, cl[i].g, cl[i].b) <
		    luma(cl[dark].r, cl[dark].g, cl[dark].b))
			dark = i;
		if (luma(cl[i].r, cl[i].g, cl[i].b) >
		    luma(cl[light].r, cl[light].g, cl[light].b))
			light = i;
	}

	pal->bg[0] = cl[dark].r;
	pal->bg[1] = cl[dark].g;
	pal->bg[2] = cl[dark].b;
	pal->fg[0] = cl[light].r;
	pal->fg[1] = cl[light].g;
	pal->fg[2] = cl[light].b;
	pal->r[0] = cl[dark].r;
	pal->g[0] = cl[dark].g;
	pal->b[0] = cl[dark].b;
	pal->weight[0] = cl[dark].weight;
	pal->n = 1;

	for (i = 0; i < ncl; i++) {
		if (i == dark || i == light)
			continue;
		sorted[n++] = cl[i];
	}

	for (i = 1; i < n; i++) {
		struct cluster key = sorted[i];
		uint64_t kw = key.weight;

		for (j = i; j > 0 && sorted[j - 1].weight < kw; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = key;
	}

	for (i = 0; i < n && pal->n + 1 < want; i++) {
		pal->r[pal->n] = sorted[i].r;
		pal->g[pal->n] = sorted[i].g;
		pal->b[pal->n] = sorted[i].b;
		pal->weight[pal->n] = sorted[i].weight;
		pal->n++;
	}

	pal->r[pal->n] = cl[light].r;
	pal->g[pal->n] = cl[light].g;
	pal->b[pal->n] = cl[light].b;
	pal->weight[pal->n] = cl[light].weight;
	pal->n++;

	while (pal->n < want) {
		size_t k = pal->n;
		uint32_t t = (uint32_t)k * 1000 / (uint32_t)(want - 1);

		pal->r[k] = (uint8_t)((cl[dark].r * (1000 - t) +
				       cl[light].r * t) / 1000);
		pal->g[k] = (uint8_t)((cl[dark].g * (1000 - t) +
				       cl[light].g * t) / 1000);
		pal->b[k] = (uint8_t)((cl[dark].b * (1000 - t) +
				       cl[light].b * t) / 1000);
		pal->weight[k] = 1;
		pal->n++;
	}
}

static void pct_str(uint64_t w, uint64_t total, char *buf, size_t len)
{
	unsigned tenths;

	if (!total) {
		snprintf(buf, len, "0.0%%");
		return;
	}
	tenths = (unsigned)(w * 1000 / total);
	if (!tenths) {
		snprintf(buf, len, ">0.1%%");
		return;
	}
	snprintf(buf, len, "%u.%u%%", tenths / 10, tenths % 10);
}

static const char *color_slot(size_t i)
{
	static char buf[8];

	if (i < 16)
		snprintf(buf, sizeof(buf), "color%zu", i);
	else
		buf[0] = '\0';
	return buf;
}

static void palette_sort_weight(struct palette *pal)
{
	size_t i, j;

	for (i = 1; i < pal->n; i++) {
		uint8_t r = pal->r[i], g = pal->g[i], b = pal->b[i];
		uint64_t w = pal->weight[i];

		for (j = i; j > 0 && pal->weight[j - 1] < w; j--) {
			pal->r[j] = pal->r[j - 1];
			pal->g[j] = pal->g[j - 1];
			pal->b[j] = pal->b[j - 1];
			pal->weight[j] = pal->weight[j - 1];
		}
		pal->r[j] = r;
		pal->g[j] = g;
		pal->b[j] = b;
		pal->weight[j] = w;
	}
}

static void palette_terminal(const struct palette *pal, uint64_t total)
{
	size_t i;
	char pct[16];

	printf("%-6s %-10s %-12s %-8s   swatch\n", "idx", "color", "hex", "pct");
	for (i = 0; i < pal->n; i++) {
		pct_str(pal->weight[i], total, pct, sizeof(pct));
		printf("%-6zu %-10s #%02x%02x%02x    %-8s   ",
		       i + 1, color_slot(i), pal->r[i], pal->g[i], pal->b[i],
		       pct);
		printf("\033[48;2;%u;%u;%um        \033[0m\n",
		       pal->r[i], pal->g[i], pal->b[i]);
	}
}

static void print_plain(const struct palette *pal)
{
	size_t i;

	for (i = 0; i < pal->n; i++)
		printf("%02x%02x%02x\n", pal->r[i], pal->g[i], pal->b[i]);
}

static void print_json(const struct palette *pal)
{
	size_t i;
	uint64_t total = 0;

	for (i = 0; i < pal->n; i++)
		total += pal->weight[i];
	printf("{\n  \"colors\": [");
	for (i = 0; i < pal->n; i++) {
		printf("%s\"%02x%02x%02x\"", i ? ", " : "",
		       pal->r[i], pal->g[i], pal->b[i]);
	}
	printf("],\n  \"weights\": [");
	for (i = 0; i < pal->n; i++) {
		printf("%s%llu", i ? ", " : "",
		       (unsigned long long)pal->weight[i]);
	}
	printf("],\n  \"background\": \"%02x%02x%02x\",\n  "
	       "\"foreground\": \"%02x%02x%02x\",\n  \"count\": %zu\n}\n",
	       pal->bg[0], pal->bg[1], pal->bg[2],
	       pal->fg[0], pal->fg[1], pal->fg[2],
	       pal->n);
}

static void print_osc(const struct palette *pal)
{
	size_t i;

	for (i = 0; i < pal->n && i < 16; i++)
		printf("\033]4;%zu;#%02x%02x%02x\033\\", i, pal->r[i],
		       pal->g[i], pal->b[i]);
	printf("\033]11;#%02x%02x%02x\033\\\033]10;#%02x%02x%02x\033\\",
	       pal->bg[0], pal->bg[1], pal->bg[2],
	       pal->fg[0], pal->fg[1], pal->fg[2]);
	printf("\033]708;#%02x%02x%02x\033\\", pal->bg[0], pal->bg[1],
	       pal->bg[2]);
}

static char *read_stream(FILE *f, size_t *len)
{
	size_t cap = 8192, n = 0;
	char *buf = xmalloc(cap);

	for (;;) {
		size_t got;

		if (n == cap - 1) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		got = fread(buf + n, 1, cap - 1 - n, f);
		n += got;
		if (got == 0) {
			if (ferror(f))
				die("read error", NULL);
			break;
		}
	}
	buf[n] = '\0';
	*len = n;
	return buf;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	size_t len;

	if (!f)
		die(strerror(errno), path);
	buf = read_stream(f, &len);
	fclose(f);
	if (!len)
		die("empty template", path);
	return buf;
}

static int palette_lookup(const struct palette *pal, const char *key,
			  size_t klen, size_t *idx)
{
	size_t i, v;

	if (klen > 5 && !strncmp(key, "color", 5)) {
		v = 0;
		for (i = 5; i < klen; i++) {
			if (key[i] < '0' || key[i] > '9')
				return 0;
			v = v * 10 + (key[i] - '0');
			if (v >= MAX_COLORS)
				return 0;
		}
		if (v >= pal->n)
			return 0;
		*idx = v;
		return 1;
	}
	return 0;
}

static void render_template(const char *src, const struct palette *pal,
			    char **out, size_t *outlen)
{
	FILE *ms = open_memstream(out, outlen);
	const char *s;

	if (!ms)
		die("out of memory", NULL);
	for (s = src; *s; s++) {
		const char *e;
		const char *key;
		size_t klen, idx;

		if (*s != '{') {
			fputc(*s, ms);
			continue;
		}
		e = strchr(s, '}');
		if (!e) {
			fputs(s, ms);
			break;
		}
		key = s + 1;
		klen = e - key;
		if (klen == 10 && !strncmp(key, "background", 10)) {
			fprintf(ms, "%02x%02x%02x",
				pal->bg[0], pal->bg[1], pal->bg[2]);
		} else if (klen == 10 && !strncmp(key, "foreground", 10)) {
			fprintf(ms, "%02x%02x%02x",
				pal->fg[0], pal->fg[1], pal->fg[2]);
		} else if (klen < 16 && palette_lookup(pal, key, klen, &idx)) {
			fprintf(ms, "%02x%02x%02x",
				pal->r[idx], pal->g[idx], pal->b[idx]);
		} else {
			fwrite(s, 1, e - s + 1, ms);
		}
		s = e;
	}
	fclose(ms);
}

static void write_file_atomic(const char *path, const char *data, size_t len)
{
	size_t plen = strlen(path);
	char *tmp = xmalloc(plen + 8);
	int fd;
	FILE *f;

	memcpy(tmp, path, plen);
	memcpy(tmp + plen, ".XXXXXX", 8);
	fd = mkstemp(tmp);
	if (fd < 0)
		die(strerror(errno), path);
	f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		unlink(tmp);
		die(strerror(errno), path);
	}
	if (fwrite(data, 1, len, f) != len || fflush(f) != 0 ||
	    fclose(f) != 0) {
		unlink(tmp);
		die("write error", path);
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		die(strerror(errno), path);
	}
	free(tmp);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static uint8_t hex2(const char *s)
{
	int hi = hexval(s[0]);
	int lo = hexval(s[1]);

	return (uint8_t)((hi < 0 ? 0 : hi) * 16 + (lo < 0 ? 0 : lo));
}

static void usage(FILE *f)
{
	fprintf(f,
		"palc %s - wallpaper palette (c) truewulf, ISC license\n\n"
		"usage: palc [options] [image]\n"
		"image defaults to the current wallpaper (auto-detected)\n\n"
		"  palc               palette of the current wallpaper\n"
		"  palc -n 8          exactly 8 colors (8..32)\n"
		"  palc -m            one hex per line, for scripts\n"
		"  palc -j            json output\n"
		"  palc -e            osc sequences for live terminal recolor\n"
		"  palc -R            reset terminals to the saved original theme\n"
		"  palc -q -T tpl -o out\n"
		"                     render template with {color0}..{color31},\n"
		"                     {background}, {foreground}\n"
		"  palc -h | -v       help | version\n",
		VERSION);
}

static void version(void)
{
	printf("palc %s\n", VERSION);
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);

	memcpy(p, s, n);
	return p;
}

static char *home_path(const char *rel)
{
	char *home = getenv("HOME");
	char path[PATH_MAX];

	if (!home || !*home)
		return NULL;
	if (rel[0] == '~')
		rel++;
	if (snprintf(path, sizeof(path), "%s%s", home, rel) >=
	    (int)sizeof(path))
		return NULL;
	return xstrdup(path);
}

static int usable_image(const char *path)
{
	static const char *ext[] = { ".png", ".jpg", ".jpeg", ".webp",
				     ".bmp", ".gif", ".tga", ".pnm",
				     ".ppm", ".svg", NULL };
	size_t i, len = strlen(path);

	if (path[0] != '/' || access(path, R_OK) != 0)
		return 0;
	for (i = 0; ext[i]; i++) {
		size_t el = strlen(ext[i]);

		if (len > el && !strcasecmp(path + len - el, ext[i]))
			return 1;
	}
	return 0;
}

static char *strip_uri(char *s)
{
	char *e = s;

	while (*s == ' ' || *s == '\'' || *s == '"')
		s++;
	if (!strncmp(s, "file://", 7))
		s += 7;
	while (*s) {
		if (*s == '%' && isxdigit((unsigned char)s[1]) &&
		    isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], 0 };

			*e++ = (char)strtoul(hex, NULL, 16);
			s += 3;
		} else if (*s == '\'' || *s == '"') {
			s++;
		} else {
			*e++ = *s++;
		}
	}
	*e = '\0';
	return e;
}

static char *conf_key(const char *path, const char *key)
{
	FILE *f = fopen(path, "rb");
	char line[PATH_MAX];
	size_t klen = strlen(key);
	char *val;

	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		char *s = line;

		while (*s == ' ')
			s++;
		if (strncmp(s, key, klen) != 0 || s[klen] != '=')
			continue;
		val = s + klen + 1;
		while (*val == ' ')
			val++;
		val[strlen(val) > 0 && val[strlen(val) - 1] == '\n'
			    ? strlen(val) - 1 : strlen(val)] = '\0';
		fclose(f);
		return xstrdup(val);
	}
	fclose(f);
	return NULL;
}

static char *detect_env(void)
{
	const char *v = getenv("PALC_WALLPAPER");

	if (!v || !*v)
		v = getenv("WALLPAPER");
	if (v && *v)
		return xstrdup(v);
	return NULL;
}

static char *detect_proc(void)
{
	static const char *daemons[] = { "swaybg", "wawa", "mpvpaper", NULL };
	DIR *d;
	struct dirent *de;
	char *found = NULL;

	d = opendir("/proc");
	if (!d)
		return NULL;
	while (!found && (de = readdir(d))) {
		char path[64], buf[8192];
		char *args[64];
		size_t nargs = 0;
		FILE *f;
		size_t got, i;
		const char *prog, *cand = NULL;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		if (snprintf(path, sizeof(path), "/proc/%s/cmdline",
			     de->d_name) >= (int)sizeof(path))
			continue;
		f = fopen(path, "rb");
		if (!f)
			continue;
		got = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[got] = '\0';
		for (i = 0; i + 1 < got && nargs < 63; i++)
			if (buf[i] == '\0')
				buf[i] = ' ';
		args[nargs++] = buf;
		for (i = 0; i < got && nargs < 63; i++) {
			if (buf[i] == ' ') {
				buf[i] = '\0';
				args[nargs++] = buf + i + 1;
			}
		}
		if (nargs == 0)
			continue;
		prog = strrchr(args[0], '/');
		prog = prog ? prog + 1 : args[0];
		for (i = 0; daemons[i]; i++) {
			if (!strcmp(prog, daemons[i]))
				break;
		}
		if (!daemons[i])
			continue;
		for (i = 1; i < nargs; i++) {
			if (!strcmp(args[i], "-i") && i + 1 < nargs)
				cand = args[i + 1];
			else if (!strcmp(args[i], "fill") && i + 1 < nargs)
				cand = args[i + 1];
			else if (!strcmp(prog, "mpvpaper") && i + 1 == nargs - 1)
				cand = args[i];
		}
		if (cand && cand[0] == '/' && usable_image(cand))
			found = xstrdup(cand);
	}
	closedir(d);
	return found;
}

static char *detect_gsettings(void)
{
	const char *cmd = "gsettings get org.gnome.desktop.background "
			  "picture-uri 2>/dev/null";
	FILE *f = popen(cmd, "r");
	char line[PATH_MAX + 8];
	char *p;

	if (!f)
		return NULL;
	if (!fgets(line, sizeof(line), f)) {
		pclose(f);
		return NULL;
	}
	pclose(f);
	p = strip_uri(line);
	return usable_image(p) ? xstrdup(p) : NULL;
}

static char *detect_configs(void)
{
	static const char *keys[][2] = {
		{ "~/.config/hypr/hyprpaper.conf", "preload" },
		{ "~/.config/waypaper/config.ini", "wallpaper" },
		{ NULL, NULL }
	};
	char path[PATH_MAX];
	char *p, *home = getenv("HOME");
	int i;

	for (i = 0; keys[i][0]; i++) {
		if (snprintf(path, sizeof(path), "%s%s", home ? home : "",
			     keys[i][0] + 1) >= (int)sizeof(path))
			continue;
		p = conf_key(path, keys[i][1]);
		if (p && usable_image(p))
			return p;
		free(p);
	}
	return NULL;
}

static char *detect_nitrogen(void)
{
	char *path = home_path("~/.nitrogen/bg-saved.cfg");
	char *p;

	if (!path)
		return NULL;
	p = conf_key(path, "file");
	free(path);
	if (p && usable_image(p))
		return p;
	free(p);
	return NULL;
}

static char *detect_fehbg(void)
{
	char *path = home_path("~/.fehbg");
	FILE *f;
	char line[PATH_MAX * 2];
	char *s, *end;

	if (!path)
		return NULL;
	f = fopen(path, "rb");
	free(path);
	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		s = strstr(line, "--bg-");
		if (!s)
			continue;
		while (*s && *s != '\'' && *s != '"')
			s++;
		if (!*s)
			continue;
		end = strchr(s + 1, *s);
		if (!end)
			continue;
		*end = '\0';
		s++;
		fclose(f);
		return usable_image(s) ? xstrdup(s) : NULL;
	}
	fclose(f);
	return NULL;
}

static char *detect_current_link(void)
{
	char *path = home_path("~/Pictures/Wallpapers/current");
	char buf[PATH_MAX];
	ssize_t n;

	if (!path)
		return NULL;
	n = readlink(path, buf, sizeof(buf) - 1);
	free(path);
	if (n <= 0)
		return NULL;
	buf[n] = '\0';
	if (buf[0] != '/') {
		char *dir = home_path("~/Pictures/Wallpapers");
		char joined[PATH_MAX];

		if (!dir)
			return NULL;
		if (snprintf(joined, sizeof(joined), "%s/%s", dir, buf) >=
		    (int)sizeof(joined)) {
			free(dir);
			return NULL;
		}
		free(dir);
		strcpy(buf, joined);
	}
	return usable_image(buf) ? xstrdup(buf) : NULL;
}

static char *detect_wallpaper(void)
{
	static char *(*steps[])(void) = {
		detect_env,     detect_proc, detect_gsettings,
		detect_configs, detect_nitrogen, detect_fehbg,
		detect_current_link, NULL
	};
	char *p;
	size_t i;

	for (i = 0; steps[i]; i++) {
		p = steps[i]();
		if (p) {
			if (usable_image(p))
				return p;
			free(p);
		}
	}
	return NULL;
}


static int do_reset(void)
{
	static const char *names[16] = {
		"color0", "color1", "color2", "color3",
		"color4", "color5", "color6", "color7",
		"color8", "color9", "color10", "color11",
		"color12", "color13", "color14", "color15"
	};
	uint8_t rr[16], gg[16], bb[16];
	int got[16] = { 0 };
	size_t k;
	char path[4096];
	const char *home = getenv("HOME");
	FILE *f;
	char line[512];
	char bg[7] = "0d0d0d", fg[7] = "eef1f8";

	if (!home)
		die("HOME is unset", NULL);
	snprintf(path, sizeof(path), "%s/.config/palc/"
		 "havoc.cfg.original", home);
	f = fopen(path, "rb");
	if (!f)
		die("no saved original theme", path);
	while (fgets(line, sizeof(line), f)) {
		char *val;
		size_t k;

		if (strncmp(line, "background", 10) == 0) {
			val = strchr(line, '#');
			if (val && strlen(val) >= 7) {
				memcpy(bg, val + 1, 6);
				bg[6] = '\0';
			}
		} else if (strncmp(line, "foreground", 10) == 0) {
			val = strchr(line, '#');
			if (val && strlen(val) >= 7) {
				memcpy(fg, val + 1, 6);
				fg[6] = '\0';
			}
		}
		for (k = 0; k < 16; k++) {
			if (strncmp(line, names[k], strlen(names[k])))
				continue;
			if (line[strlen(names[k])] != ' ' &&
			    line[strlen(names[k])] != '\t')
				continue;
			val = strchr(line, '#');
			if (!val || strlen(val) < 7)
				continue;
			rr[k] = hex2(val + 1);
			gg[k] = hex2(val + 3);
			bb[k] = hex2(val + 5);
			got[k] = 1;
		}
	}
	fclose(f);
	for (k = 0; k < 16; k++) {
		if (got[k])
			printf("\033]4;%zu;#%02x%02x%02x\033\\",
			       k, rr[k], gg[k], bb[k]);
	}
	printf("\033]11;#%s\033\\\033]10;#%s\033\\", bg, fg);

	snprintf(path, sizeof(path), "%s/.config/havoc/havoc.cfg", home);
	{
		char origpath[4096];
		char *orig;

		snprintf(origpath, sizeof(origpath), "%s/.config/palc/"
			 "havoc.cfg.original", home);
		orig = read_file(origpath);
		write_file_atomic(path, orig, strlen(orig));
		free(orig);
	}

	{
		DIR *d = opendir("/proc");
		struct dirent *de;

		if (d) {
			while ((de = readdir(d))) {
				char lp[32], tgt[PATH_MAX];
				ssize_t n;

				if (de->d_name[0] < '0' || de->d_name[0] > '9')
					continue;
				snprintf(lp, sizeof(lp), "/proc/%s/exe",
					 de->d_name);
				n = readlink(lp, tgt, sizeof(tgt) - 1);
				if (n <= 0)
					continue;
				tgt[n] = '\0';
				if (strstr(tgt, " (deleted)"))
					continue;
				if (!strstr(tgt, "havoc"))
					continue;
				kill((pid_t)atoi(de->d_name), SIGUSR1);
			}
			closedir(d);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *templates[MAX_TEMPLATES];
	const char *outputs[MAX_TEMPLATES];
	size_t ntemplates = 0, noutputs = 0;
	struct cluster cl[MAX_CLUSTERS];
	struct palette pal;
	uint32_t *px;
	size_t npx, ncl, i;
	int want = DEFAULT_COLORS, mode = MODE_TERM, quiet = 0, opt;
	char *image;

	while ((opt = getopt(argc, argv, "n:jmeRqT:o:hv")) != -1) {
		char *end;

		switch (opt) {
		case 'n':
			want = strtol(optarg, &end, 10);
			if (*end || want < 8 || want > MAX_COLORS)
				die("invalid color count (8..32)", optarg);
			break;
		case 'j':
			mode = MODE_JSON;
			break;
		case 'm':
			mode = MODE_PLAIN;
			break;
		case 'e':
			mode = MODE_OSC;
			break;
		case 'R':
			mode = MODE_RESET;
			break;
		case 'T':
			if (ntemplates == MAX_TEMPLATES)
				die("too many templates", optarg);
			templates[ntemplates++] = optarg;
			break;
		case 'o':
			if (noutputs >= ntemplates || noutputs == MAX_TEMPLATES)
				die("-o without -T", optarg);
			outputs[noutputs++] = optarg;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		case 'v':
			version();
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (mode == MODE_RESET)
		return do_reset();

	if (optind == argc - 1) {
		image = xstrdup(argv[optind]);
	} else if (optind == argc) {
		image = detect_wallpaper();
		if (!image)
			die("cannot detect current wallpaper", NULL);
	} else {
		usage(stderr);
		return 2;
	}

	load_samples(image, &px, &npx);
	ncl = cluster_pixels(px, npx, want ? want - 2 : MAX_CLUSTERS, cl);
	free(px);
	if (!want) {
		want = (int)ncl + 2;
		if (want > MAX_TERMINAL)
			want = MAX_TERMINAL;
	}
	build_palette(cl, ncl, want, &pal);
	palette_sort_weight(&pal);

	for (i = 0; i < ntemplates; i++) {
		if (!outputs[i])
			die("-T without -o", templates[i]);
	}

	if (!quiet) {
		if (mode == MODE_JSON)
			print_json(&pal);
		else if (mode == MODE_PLAIN)
			print_plain(&pal);
		else if (mode == MODE_OSC)
			print_osc(&pal);
		else {
			uint64_t total = 0;

			for (i = 0; i < pal.n; i++)
				total += pal.weight[i];
			palette_terminal(&pal, total);
		}
	}
	for (i = 0; i < ntemplates; i++) {
		char *src = read_file(templates[i]);
		char *out;
		size_t outlen;

		render_template(src, &pal, &out, &outlen);
		write_file_atomic(outputs[i], out, outlen);
		free(src);
		free(out);
	}
	return 0;
}
