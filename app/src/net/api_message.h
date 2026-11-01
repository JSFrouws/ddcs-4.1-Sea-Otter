/* HMI message: an instruction for the person at the machine, posted over the API (owner
 * 2026-11-01). One at a time, RAM only. Drawn on every screen until it is acknowledged with
 * Shift+F7; the record (acked, when) stays readable until the next POST or DELETE. */
#ifndef API_MESSAGE_H
#define API_MESSAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HMSG_TEXT 600
#define HMSG_BUTTON 32
typedef struct {
	unsigned id;                         /* 0 = none */
	char text[HMSG_TEXT], button[HMSG_BUTTON];
	uint64_t posted_ms, acked_ms;        /* now_ms(): the controller's uptime clock */
	bool acked;
} hmsg_t;
extern hmsg_t HMSG;

bool hmsg_pending(void);                 /* posted and not acknowledged: the box is up */
bool hmsg_ack(void);                     /* Shift+F7; false when nothing was pending */
void hmsg_json(char *out, size_t n, bool full);   /* the object for GET message (full) / status */
/* GET / POST / DELETE /api/v1/message: returns the HTTP status, JSON in out */
int api_message(const char *method, const char *body, char *out, size_t n);

#endif
