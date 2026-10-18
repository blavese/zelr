#pragma once
#include "types.h"

/* The clock set from the network.
 *
 * A machine whose CMOS clock is wrong -- a flat battery puts it back years,
 * and nothing here ever set it -- fails every certificate check, since a
 * certificate is valid only between two dates, and dates every file wrongly.
 * So once the network is up the machine asks a time server (SNTP, RFC 4330;
 * pool.ntp.org unless /zelr.cfg has a "timeserver NAME[:PORT]" line), and
 * when the theme's clock_net is on (it is unless somebody turns it off) the
 * answer becomes an adjustment that zone.h adds to every reading of the
 * clock. The CMOS clock itself is never written.
 *
 * The answer is not authenticated (no NTS), as SNTP's is not anywhere: what
 * keeps a stray packet out is the nonce net.c checks, not a signature. */

/* What a time server answered, read: the seconds since 1970 it gives, or
   false and why not (a short answer, one not from a server or not to this
   question, a server that says its own clock is not set or refuses). */
bool timesync_read(const u8 *reply, u32 len, const u8 nonce[8], i64 *unix_out, const char **why);

/* Asks SERVER ("name", "name:port" or an address) and sets the clock by the
   answer. False, and the clock left alone, when there is no answer to take.
   Waits for the network, so it is not for a caller with interrupts off. */
bool timesync_now(const char *server);

/* What the last asking came to, for /sys/time and the shell: "set from
   pool.ntp.org, the clock was 3 s slow", or why not. */
const char *timesync_said(void);
