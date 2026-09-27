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

/* Forgets the session without telling the other end, for a connection whose
   program has been ended from outside. Does not wait. */
void tls_abandon(int tcp);

/* The order the server's encrypted handshake must come in: extensions,
   certificate, signature, finished, each once. Given the step reached and the
   type of the next message, returns the step after it, or -1 with the reason
   in *why. TLS_FLIGHT_DONE means finished has arrived. */
#define TLS_FLIGHT_START 0
#define TLS_FLIGHT_DONE  4
int tls_flight_step(int step, u8 type, const char **why);

/* For the self test, with buffers in place of the network. The bytes of the
   fatal alert a session would send, sealed under the key from `secret` or in
   the clear if it is null. */
u32 tls_test_alert(const u8 *secret, u8 desc, u8 *out, u32 cap);

/* How many writes to TCP the last of those took: one a record. */
u32 tls_test_writes(void);

/* A session in mid conversation under these application secrets, handed
   `in` as what arrived: what it gives back as data, what it sent, and the
   secrets it ended with. */
u32 tls_test_recv(const u8 s_app[32], const u8 c_app[32], const u8 *in, u32 in_len,
                  u8 *got, u32 got_cap, u8 *sent, u32 sent_cap, u32 *sent_len,
                  u8 s_after[32], u8 c_after[32]);

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
