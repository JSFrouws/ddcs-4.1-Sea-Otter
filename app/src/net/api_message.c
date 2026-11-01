/* HMI message (api_message.h). Kept out of api.c so the host tests can drive it without
 * the HTTP server: api_message() answers with a status and a JSON text, api.c sends it. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "api_message.h"
#include "../ui/ui.h"

hmsg_t HMSG;
static unsigned last_id;                 /* monotonic while the app runs (RAM only) */

bool hmsg_pending(void) { return HMSG.id && !HMSG.acked; }

bool hmsg_ack(void)
{
	if (!hmsg_pending()) return false;
	HMSG.acked = true; HMSG.acked_ms = now_ms(); U.dirty = true;
	return true;
}

/* utf-8 for one code point (the fonts are BMP only) */
static size_t put_utf8(char *o, unsigned c)
{
	if (c < 0x80) { o[0] = (char)c; return 1; }
	if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
	o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3;
}

/* the string value of "key" in a flat JSON body, escapes decoded (api.c's jstr keeps "\n" as
 * a plain n, and a message needs its line breaks). 0 = found, -1 = absent, -2 = too long. */
static int jtext(const char *b, const char *key, char *out, size_t n)
{
	char k[24];
	snprintf(k, sizeof k, "\"%s\"", key);
	const char *p = b ? strstr(b, k) : NULL;
	if (!p || !(p = strchr(p + strlen(k), ':'))) return -1;
	for (p++; *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'; p++) ;
	if (*p != '"') return -1;
	size_t i = 0;
	for (p++; *p && *p != '"'; ) {
		char t[4]; size_t m;
		unsigned c = (unsigned char)*p++;
		bool raw = c >= 0x80;                                    /* a utf-8 byte: copied as is */
		if (c == '\\' && *p) {
			c = (unsigned char)*p++;
			if (c == 'n') c = '\n';
			else if (c == 't' || c == 'r' || c == 'b' || c == 'f') c = ' ';
			else if (c == 'u') {
				unsigned v = 0; int d = 0;
				for (; d < 4 && *p; d++, p++) {
					char h = *p;
					v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : 0);
				}
				c = v >= 0xD800 && v < 0xE000 ? '?' : v < 32 && v != '\n' ? ' ' : v;
			}
		} else if (c < 32 && c != '\n') c = ' ';
		if (raw) { t[0] = (char)c; m = 1; }
		else m = put_utf8(t, c);
		if (i + m >= n) return -2;
		memcpy(out + i, t, m); i += m;
	}
	out[i] = 0;
	return 0;
}

static void esc(char *o, size_t n, const char *s)
{
	size_t k = 0;
	for (; *s && k + 3 < n; s++) {
		if (*s == '\n') { o[k++] = '\\'; o[k++] = 'n'; continue; }
		if (*s == '"' || *s == '\\') o[k++] = '\\';
		o[k++] = (unsigned char)*s < 32 ? ' ' : *s;
	}
	o[k] = 0;
}

void hmsg_json(char *out, size_t n, bool full)
{
	char t[HMSG_TEXT * 2 + 8], bt[HMSG_BUTTON * 2 + 8];
	if (!HMSG.id) { snprintf(out, n, "{\"id\":0}"); return; }
	esc(bt, sizeof bt, HMSG.button);
	if (!full) { snprintf(out, n, "{\"id\":%u,\"acked\":%s,\"button\":\"%s\"}", HMSG.id, HMSG.acked ? "true" : "false", bt); return; }
	esc(t, sizeof t, HMSG.text);
	snprintf(out, n, "{\"id\":%u,\"text\":\"%s\",\"button\":\"%s\",\"postedMs\":%llu,\"acked\":%s,\"ackedMs\":%llu,\"ageMs\":%llu}",
		HMSG.id, t, bt, (unsigned long long)HMSG.posted_ms, HMSG.acked ? "true" : "false",
		(unsigned long long)(HMSG.acked ? HMSG.acked_ms : 0), (unsigned long long)(now_ms() - HMSG.posted_ms));
}

int api_message(const char *method, const char *body, char *out, size_t n)
{
	if (!strcasecmp(method, "GET")) { hmsg_json(out, n, true); return 200; }
	if (!strcasecmp(method, "DELETE")) {
		bool had = HMSG.id != 0;
		memset(&HMSG, 0, sizeof HMSG); U.dirty = true;
		snprintf(out, n, "{\"ok\":true,\"cleared\":%s}", had ? "true" : "false");
		return 200;
	}
	if (strcasecmp(method, "POST") && strcasecmp(method, "PUT")) { snprintf(out, n, "{\"error\":\"method\"}"); return 405; }
	char text[HMSG_TEXT], button[HMSG_BUTTON];
	int e = jtext(body, "text", text, sizeof text);
	if (e == -2) { snprintf(out, n, "{\"error\":\"too_long\",\"detail\":\"text: at most %d bytes\"}", HMSG_TEXT - 1); return 400; }
	if (e || !text[strspn(text, " \n")]) { snprintf(out, n, "{\"error\":\"need text\"}"); return 400; }
	e = jtext(body, "button", button, sizeof button);
	if (e == -2) { snprintf(out, n, "{\"error\":\"too_long\",\"detail\":\"button: at most %d bytes\"}", HMSG_BUTTON - 1); return 400; }
	if (e || !button[strspn(button, " \n")]) snprintf(button, sizeof button, "OK");
	for (char *c = button; *c; c++) if (*c == '\n') *c = ' ';   /* one line on the key */
	memset(&HMSG, 0, sizeof HMSG);                               /* replaces any pending one */
	HMSG.id = ++last_id; HMSG.posted_ms = now_ms();
	memcpy(HMSG.text, text, sizeof text); memcpy(HMSG.button, button, sizeof button);
	U.dirty = true;
	snprintf(out, n, "{\"ok\":true,\"id\":%u}", HMSG.id);
	return 200;
}
