#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "macro.h"
#include "machine.h"

char macro_dir_sys[256], macro_dir_user[256];

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
	size_t i = 0;
	while (s[i] && isspace((unsigned char)s[i])) i++;
	if (i) memmove(s, s + i, n - i + 1);
}

/* the (name: ...) (info: ...) (needs: ...) header comments */
static void read_header(macro_t *m)
{
	FILE *f = fopen(m->path, "r");
	if (!f) return;
	char line[200];
	for (int k = 0; k < 12 && fgets(line, sizeof line, f); k++) {
		trim(line);
		if (line[0] != '(') { if (line[0] && line[0] != ';' && line[0] != '%') break; continue; }
		char *e = strrchr(line, ')');
		if (e) *e = 0;
		char *v = strchr(line, ':');
		if (!v) continue;
		*v++ = 0;
		trim(v);
		char *key = line + 1;
		trim(key);
		if (!strcasecmp(key, "name")) snprintf(m->name, sizeof m->name, "%s", v);
		else if (!strcasecmp(key, "info")) snprintf(m->info, sizeof m->info, "%s", v);
		else if (!strcasecmp(key, "needs") && strstr(v, "homed")) m->needs_homed = true;
	}
	fclose(f);
}

static int ends_nc(const char *n)
{
	size_t l = strlen(n);
	return l > 3 && !strcasecmp(n + l - 3, ".nc");
}

struct scan { const char *dir, *prefix; bool user; macro_t *list; int n, max; };

static void scan_one(const char *name, void *ctx)
{
	struct scan *sc = ctx;
	if (sc->n >= sc->max || name[0] == '.' || !ends_nc(name)) return;
	if (sc->prefix && strncasecmp(name, sc->prefix, strlen(sc->prefix))) return;
	macro_t *m = &sc->list[sc->n];
	memset(m, 0, sizeof *m);
	snprintf(m->path, sizeof m->path, "%.150s/%.100s", sc->dir, name);
	/* default name: the file name without "macro-" and ".nc", dashes as spaces */
	const char *b = name + (sc->prefix ? strlen(sc->prefix) : 0);
	snprintf(m->name, sizeof m->name, "%.*s", (int)(strlen(b) - 3), b);
	for (char *c = m->name; *c; c++) if (*c == '-' || *c == '_') *c = ' ';
	m->user = sc->user;
	read_header(m);
	sc->n++;
}

static int scan_dir(const char *dir, const char *prefix, bool user, macro_t *list, int n, int max)
{
	if (!dir[0]) return n;
	struct scan sc = { dir, prefix, user, list, n, max };
	dir_list(dir, scan_one, &sc);
	return sc.n;
}

int macro_scan(macro_t *list, int max)
{
	int n = scan_dir(macro_dir_sys, "macro-", false, list, 0, max);
	n = scan_dir(macro_dir_user, NULL, true, list, n, max);
	for (int i = 1; i < n; i++)                                   /* by name */
		for (int j = i; j > 0 && strcasecmp(list[j - 1].name, list[j].name) > 0; j--) {
			macro_t t = list[j]; list[j] = list[j - 1]; list[j - 1] = t;
		}
	return n;
}

int macro_find(const char *key, macro_t *out)
{
	static macro_t all[MACRO_MAX];
	int n = macro_scan(all, MACRO_MAX);
	for (int i = 0; i < n; i++) {
		const char *base = strrchr(all[i].path, '/');
		base = base ? base + 1 : all[i].path;
		if (!strcmp(all[i].path, key) || !strcasecmp(all[i].name, key) || !strcasecmp(base, key)) { *out = all[i]; return 0; }
	}
	return -1;
}

/* a name or a number -> value */
static int value_of(const char *s, double *v)
{
	char *end;
	*v = strtod(s, &end);
	if (end != s && !*end) return 0;
	static const char ax[] = "xyza";
	for (int a = 0; a < NAXES; a++) {
		char k[16];
		snprintf(k, sizeof k, "machine.%c", ax[a]);
		if (!strcasecmp(s, k)) { *v = C.mo.pos[a]; return 0; }
		snprintf(k, sizeof k, "work.%c", ax[a]);
		if (!strcasecmp(s, k)) { *v = ctl_work(a); return 0; }
	}
	const setting_desc_t *d = setting_find(s);
	if (d) { *v = setting_get(d); return 0; }
	return -1;
}

int macro_expand(const char *src, size_t n, char *out, size_t cap, char *err, size_t errsz)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		if (src[i] != '{') { if (o + 1 >= cap) goto full; out[o++] = src[i]; continue; }
		size_t j = i + 1;
		while (j < n && src[j] != '}' && src[j] != '\n' && j - i < 80) j++;
		if (j >= n || src[j] != '}') { snprintf(err, errsz, "unclosed { in the macro"); return -1; }
		char tok[96];
		snprintf(tok, sizeof tok, "%.*s", (int)(j - i - 1), src + i + 1);
		trim(tok);
		double v;
		if (!strncasecmp(tok, "max:", 4) || !strncasecmp(tok, "min:", 4)) {
			char *a = tok + 4, *b = strchr(a, ',');
			double va, vb;
			if (!b) { snprintf(err, errsz, "{%s}: needs two values, like {max:work.z,jog.safe_z}", tok); return -1; }
			*b++ = 0; trim(a); trim(b);
			if (value_of(a, &va) || value_of(b, &vb)) { snprintf(err, errsz, "{%s,%s}: unknown name", a, b); return -1; }
			v = tolower((unsigned char)tok[1]) == 'a' ? (va > vb ? va : vb) : (va < vb ? va : vb);
		} else if (value_of(tok, &v)) { snprintf(err, errsz, "{%s}: unknown name (a setting key, machine.x, work.z, ...)", tok); return -1; }
		int k = snprintf(out + o, cap - o, "%.4f", v);
		if (k < 0 || o + (size_t)k >= cap) goto full;
		o += (size_t)k;
		i = j;
	}
	out[o] = 0;
	return (int)o;
full:
	snprintf(err, errsz, "macro too large");
	return -1;
}

char *macro_expand_file(const char *path, size_t *len, char *err, size_t errsz)
{
	FILE *f = fopen(path, "rb");
	if (!f) { snprintf(err, errsz, "cannot open %s", path); return NULL; }
	char *src = malloc(65536);
	size_t n = src ? fread(src, 1, 65535, f) : 0;
	fclose(f);
	if (!src) { snprintf(err, errsz, "out of memory"); return NULL; }
	if (n == 65535) { free(src); snprintf(err, errsz, "macro files are limited to 64 kB"); return NULL; }
	size_t cap = n * 2 + 4096;
	char *out = malloc(cap);
	int r = out ? macro_expand(src, n, out, cap, err, errsz) : -1;
	free(src);
	if (r < 0) { free(out); return NULL; }
	*len = (size_t)r;
	return out;
}
