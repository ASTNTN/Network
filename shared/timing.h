/*
 * Timing parameters shared by client and server, kept together for tuning.
 * All values are in milliseconds.
 */
#ifndef TIMING_H
#define TIMING_H

// How often each peer sends KEEPALIVE.
#define KEEPALIVE_INTERVAL_MS 1000

// A peer that has sent nothing for this long is considered dead.
#define KEEPALIVE_TIMEOUT_MS  60000

// Once a message's first byte arrives, the rest must arrive within this
// time, or the connection is considered dead.
#define MESSAGE_TIMEOUT_MS    10000

// Longest wait in poll() before a thread checks its other duties
// (outbox flags, ownership, clocks, keepalives).
#define POLL_MS               50

#endif
