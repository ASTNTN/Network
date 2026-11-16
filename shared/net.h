/*
 * Message framing and socket helpers shared by client and server.
 *
 * Reading follows the framing rule in docs/protocol_blueprint.md: the
 * header byte gives the message type, PAYLOAD_SIZE[] the fixed payload,
 * and payload_extra() the bytes that follow START's prefix.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include "protocol.h"

// Milliseconds from a monotonic clock.
long long now_ms(void);

// Send one whole message (header + payload). Blocks until it is sent.
// Returns 0, or -1 if the connection failed.
int send_msg(int fd, u8 type, const void *payload, size_t len);

// As send_msg(), but never blocks: returns -1 if it can't send at once.
int send_msg_nowait(int fd, u8 type, const void *payload, size_t len);

// One complete message.
struct Msg {
	u8 type;
	size_t len;                  // Payload bytes
	u8 payload[START_MAX_SIZE];
};

// Accumulates bytes from a stream socket and splits them into messages.
struct Reader {
	u8 data[4096];
	size_t len;
	long long first_byte_ms; // When the oldest unread byte arrived
};

enum ReadStatus {
	READ_NONE,       // No complete message buffered
	READ_MSG,        // A message was written to *out
	READ_BAD_TYPE,   // The next header byte is >= MSG_COUNT (fatal)
	READ_BAD_LENGTH  // A START prefix announced more than START_MAX_PIECES placements (fatal);
	                 // out->type and the prefix in out->payload are filled in
};

void reader_init(struct Reader *r);

// Read whatever the socket has without blocking. Returns the number of
// bytes read (0 if none were waiting), or -1 on EOF or a socket error.
int reader_fill(struct Reader *r, int fd);

// Take the next complete message from the buffer.
enum ReadStatus reader_next(struct Reader *r, struct Msg *out);

// Whether a partially received message has waited longer than
// MESSAGE_TIMEOUT_MS since its first byte.
int reader_expired(const struct Reader *r, long long now);

// Short human-readable name of a message type.
const char *msg_name(u8 type);

// Human-readable description of an ERROR code.
const char *error_str(u8 code);

#endif
