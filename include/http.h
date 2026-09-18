#pragma once
#include "types.h"

#define HTTP_ERR_RESOLVE -1
#define HTTP_ERR_CONNECT -2
#define HTTP_ERR_SEND    -3
#define HTTP_ERR_MEMORY  -4
#define HTTP_ERR_EMPTY   -5
#define HTTP_ERR_TOOLONG -6
#define HTTP_ERR_TLS     -7     /* it would not prove who it was */

/* Returns the HTTP status code, or one of the negatives above. */
int http_get(const char *host, const char *path, const char *save_as);
/* `host` may carry a scheme and a port: "name", "name:port",
   "https://name". A bare name is http, because what this is usually
   pointed at is a machine on the same desk. */
