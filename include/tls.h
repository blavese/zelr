#pragma once
#include "types.h"

/* TLS 1.3, client side, over a TCP connection that is already open.
 *
 * One session at a time, which the stack underneath is no longer: it holds
 * several connections now, so a session is told which of them it is running
 * over. What is still single is the session state in this file, so a
 * machine can have one encrypted connection and several plain ones at
 * once.
 *
 * Only version 1.3 and only one cipher suite: AES-128-GCM with SHA-256,
 * which every server is required to implement, agreed over X25519. That is
 * a deliberate floor rather than a subset chosen for convenience. Older
 * versions of this protocol have a long history of being talked down into
 * by anybody in the middle, and the way that is prevented is by not
 * implementing them, not by preferring against them. A server too old for
 * 1.3 does not get a weaker connection here; it gets no connection. */

/* Runs the whole handshake over a connection that is already open, given
   by handle, including checking the certificate chain against the host name
   asked for. False means no connection: the reason is in tls_error and is
   meant to be shown to somebody. */
bool tls_connect(int tcp, const char *host);

bool tls_send(const void *data, u32 len);
u32  tls_recv(u8 *out, u32 cap, u32 timeout_ms);
bool tls_ended(void);
void tls_close(void);
bool tls_active(void);

/* Why the last thing failed. Never empty after a false. */
const char *tls_error(void);

/* What was agreed, for anything that wants to say so. */
const char *tls_describe(void);
