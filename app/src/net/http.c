#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http.h"

#define MAXC 8
#define MAX_REQ (8u << 20)      /* program uploads */

typedef struct {
	int fd;
	char *in; size_t in_len, in_cap;
	char *out; size_t out_len, out_off;
	const void *ext; size_t ext_len, ext_off;   /* static body written after the headers */
} conn_t;

static int lfd = -1;
static conn_t cs[MAXC];
static http_handler_t handler;

void resp_text(http_resp_t *r, int status, const char *type, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	r->body = malloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(r->body, (size_t)n + 1, fmt, ap);
	va_end(ap);
	r->len = (size_t)n; r->status = status; r->type = type;
}

int http_init(int port, http_handler_t h)
{
	handler = h;
	for (int i = 0; i < MAXC; i++) cs[i].fd = -1;
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) return -1;
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) { close(lfd); lfd = -1; return -1; }
	fcntl(lfd, F_SETFL, O_NONBLOCK);
	return 0;
}

static void drop(conn_t *c)
{
	close(c->fd); c->fd = -1;
	free(c->in); free(c->out);
	memset(c, 0, sizeof *c); c->fd = -1;
}

static const char *status_text(int s)
{
	switch (s) { case 200: return "OK"; case 202: return "Accepted"; case 400: return "Bad Request"; case 403: return "Forbidden";
	case 404: return "Not Found"; case 409: return "Conflict"; case 413: return "Too Large"; default: return "Error"; }
}

/* request complete? -> dispatch */
static void try_dispatch(conn_t *c)
{
	char *he = memmem(c->in, c->in_len, "\r\n\r\n", 4);
	if (!he) return;
	size_t hl = (size_t)(he - c->in) + 4, clen = 0;
	*he = 0;
	char *cl = strcasestr(c->in, "\r\nContent-Length:");
	if (cl) clen = strtoul(cl + 17, NULL, 10);
	if (clen > MAX_REQ) { c->out = strdup("HTTP/1.0 413 Too Large\r\nContent-Length: 0\r\n\r\n"); c->out_len = strlen(c->out); return; }
	if (c->in_len < hl + clen) { *he = '\r'; return; }
	char *auth = strcasestr(c->in, "\r\nAuthorization:");
	char *sp1 = strchr(c->in, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
	if (!sp1 || !sp2) { drop(c); return; }
	*sp1 = 0; *sp2 = 0;
	char *q = strchr(sp1 + 1, '?');
	if (q) *q++ = 0;
	if (auth) { auth += 16; while (*auth == ' ') auth++; char *e = strstr(auth, "\r\n"); if (e) *e = 0; }
	c->in[hl + clen] = 0;                                /* in_len < in_cap always */
	http_req_t rq = { c->in, sp1 + 1, q ? q : "", c->in + hl, clen, auth };
	http_resp_t rs = { 404, "text/plain", NULL, 0, NULL, NULL };
	handler(&rq, &rs);
	size_t blen = rs.static_body ? rs.len : (rs.body ? rs.len : 0);
	char hdr[512];
	int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n%s\r\n",
		rs.status, status_text(rs.status), rs.type ? rs.type : "text/plain", blen, rs.extra_headers ? rs.extra_headers : "");
	if (rs.static_body) {
		c->out = malloc((size_t)n); memcpy(c->out, hdr, (size_t)n); c->out_len = (size_t)n;
		c->ext = rs.static_body; c->ext_len = blen;
	} else {
		c->out = malloc((size_t)n + blen);
		memcpy(c->out, hdr, (size_t)n);
		if (blen) memcpy(c->out + n, rs.body, blen);
		c->out_len = (size_t)n + blen;
		free(rs.body);
	}
}

void http_poll(int timeout_ms)
{
	if (lfd < 0) return;
	struct pollfd p[MAXC + 1];
	int n = 0;
	p[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
	for (int i = 0; i < MAXC; i++) if (cs[i].fd >= 0) p[n++] = (struct pollfd){ .fd = cs[i].fd, .events = cs[i].out ? POLLOUT : POLLIN };
	if (poll(p, (nfds_t)n, timeout_ms) <= 0) return;
	if (p[0].revents & POLLIN) {
		int fd = accept(lfd, NULL, NULL);
		if (fd >= 0) {
			int slot = -1;
			for (int i = 0; i < MAXC; i++) if (cs[i].fd < 0) { slot = i; break; }
			if (slot < 0) close(fd);
			else { fcntl(fd, F_SETFL, O_NONBLOCK); memset(&cs[slot], 0, sizeof cs[slot]); cs[slot].fd = fd; }
		}
	}
	for (int i = 0; i < MAXC; i++) {
		conn_t *c = &cs[i];
		if (c->fd < 0) continue;
		if (!c->out) {
			if (c->in_cap - c->in_len < 65536) {
				size_t cap = c->in_cap ? c->in_cap * 2 : 131072;
				if (cap > MAX_REQ + 65536) { drop(c); continue; }
				c->in = realloc(c->in, cap); c->in_cap = cap;
			}
			ssize_t r = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len - 1);
			if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { drop(c); continue; }
			if (r > 0) { c->in_len += (size_t)r; c->in[c->in_len] = 0; try_dispatch(c); }
			if (c->fd < 0 || !c->out) continue;
		}
		for (int k = 0; k < 8; k++) {                       /* bounded work per poll */
			ssize_t w;
			if (c->out_off < c->out_len) w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
			else if (c->ext_off < c->ext_len) w = write(c->fd, (const char *)c->ext + c->ext_off, c->ext_len - c->ext_off);
			else { drop(c); break; }
			if (w < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) drop(c); break; }
			if (c->out_off < c->out_len) c->out_off += (size_t)w; else c->ext_off += (size_t)w;
		}
	}
}
