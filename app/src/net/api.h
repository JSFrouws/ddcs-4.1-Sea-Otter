#ifndef API_H
#define API_H
#include "http.h"
#include "../hal/display.h"
#include "../hal/panel.h"

void api_bind(display_t *d, panel_t *p);
void api_handle(const http_req_t *q, http_resp_t *r);

#endif
