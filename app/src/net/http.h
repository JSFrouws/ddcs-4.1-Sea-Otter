/* Minimal single-threaded, non-blocking HTTP/1.0 server, polled from the main loop so
 * a slow client can never stall the 10 ms motion tick. One request per connection. */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

typedef struct {
	const char *method, *path, *query;
	const char *body; size_t body_len;
	const char *auth;        /* Authorization header value or NULL */
} http_req_t;

typedef struct {
	int status;
	const char *type;
	char *body; size_t len;  /* malloc'd, freed by the server */
	const char *extra_headers;
	const void *static_body; /* used instead of body when set (not freed) */
} http_resp_t;

typedef void (*http_handler_t)(const http_req_t *, http_resp_t *);

int http_init(int port, http_handler_t h);   /* -1 on failure */
void http_poll(int timeout_ms);              /* accept / read / dispatch / write */
void resp_text(http_resp_t *r, int status, const char *type, const char *fmt, ...);

#endif
