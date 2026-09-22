#pragma once
#include "types.h"

/* TLS 1.3, client side, over a TCP connection that is already open.
 *
 * One session per connection, and the connection's own handle names it.
 * There is no second handle space to keep in step: a session belongs to
 * exactly one connection and a connection carries at most one session.
 *
 * It was one session for the whole machine until now, and that was not a
 * simplification of the interface -- it was file level state, the same
 * mistake the stack underneath had and fixed. What it cost was the thing
 * this is chiefly used for: a page and its pictures were fetched one
 * encrypted connection after another, each one a fresh handshake waited
 * out in turn, because there was nowhere to put a second.
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

bool tls_send(int tcp, const void *data, u32 len);
u32  tls_recv(int tcp, u8 *out, u32 cap, u32 timeout_ms);
bool tls_ended(int tcp);
void tls_close(int tcp);
bool tls_active(int tcp);

/* Whether anything at all is encrypted just now. A question about the
   machine rather than about one connection, which is what a self test
   asking "is anything open" wants to know. */
bool tls_any(void);

/* Why the last thing on this connection failed. Never empty after a false.
   A connection that was never valid -- tls_connect(-1) -- still has to be
   able to say why it was refused, so asking with the same handle gets the
   answer back. */
const char *tls_error(int tcp);

/* What was agreed, for anything that wants to say so. */
const char *tls_describe(int tcp);
