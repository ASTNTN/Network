#include "net.h"
#include "timing.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int send_flags(int fd, u8 type, const void *payload, size_t len, int flags)
{
	u8 buf[1 + START_MAX_SIZE];
	if (len > START_MAX_SIZE)
		return -1;
	buf[0] = type;
	if (len)
		memcpy(buf + 1, payload, len);

	size_t total = len + 1, sent = 0;
	while (sent < total) {
		ssize_t n = send(fd, buf + sent, total - sent, MSG_NOSIGNAL | flags);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		// A partial non-blocking send can't be finished without blocking
		if ((flags & MSG_DONTWAIT) && (size_t)n < total - sent)
			return -1;
		sent += (size_t)n;
	}
	return 0;
}

int send_msg(int fd, u8 type, const void *payload, size_t len)
{
	return send_flags(fd, type, payload, len, 0);
}

int send_msg_nowait(int fd, u8 type, const void *payload, size_t len)
{
	return send_flags(fd, type, payload, len, MSG_DONTWAIT);
}

void reader_init(struct Reader *r)
{
	r->len = 0;
	r->first_byte_ms = 0;
}

int reader_fill(struct Reader *r, int fd)
{
	if (r->len == sizeof r->data)
		return 0; // Full: the caller must take messages first
	ssize_t n;
	do
		n = recv(fd, r->data + r->len, sizeof r->data - r->len, MSG_DONTWAIT);
	while (n < 0 && errno == EINTR);

	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
	if (n == 0)
		return -1; // EOF
	if (r->len == 0)
		r->first_byte_ms = now_ms();
	r->len += (size_t)n;
	return (int)n;
}

enum ReadStatus reader_next(struct Reader *r, struct Msg *out)
{
	if (r->len == 0)
		return READ_NONE;

	u8 type = r->data[0];
	if (type >= MSG_COUNT)
		return READ_BAD_TYPE;

	size_t need = 1 + PAYLOAD_SIZE[type];
	if (r->len < need)
		return READ_NONE;

	int extra = payload_extra(type, r->data + 1);
	if (extra < 0) {
		out->type = type;
		out->len = PAYLOAD_SIZE[type];
		memcpy(out->payload, r->data + 1, out->len);
		return READ_BAD_LENGTH;
	}
	need += (size_t)extra;
	if (r->len < need)
		return READ_NONE;

	out->type = type;
	out->len = need - 1;
	memcpy(out->payload, r->data + 1, out->len);
	memmove(r->data, r->data + need, r->len - need);
	r->len -= need;
	if (r->len)
		r->first_byte_ms = now_ms(); // The rest belongs to a new message
	return READ_MSG;
}

int reader_expired(const struct Reader *r, long long now)
{
	return r->len > 0 && now - r->first_byte_ms >= MESSAGE_TIMEOUT_MS;
}

const char *msg_name(u8 type)
{
	static const char *const names[MSG_COUNT] = {
		[MSG_KEEPALIVE] = "KEEPALIVE", [MSG_ERROR] = "ERROR",   [MSG_CONNECT] = "CONNECT",
		[MSG_ACCEPT] = "ACCEPT",       [MSG_START] = "START",   [MSG_GO] = "GO",
		[MSG_MOVE] = "MOVE",           [MSG_UPDATE] = "UPDATE", [MSG_END] = "END",
		[MSG_DISCONNECT] = "DISCONNECT", [MSG_DRAW] = "DRAW",
	};
	return type < MSG_COUNT ? names[type] : "unknown";
}

const char *error_str(u8 code)
{
	switch (code) {
	case ERR_UNEXPECTED_MESSAGE:    return "message not valid at this point";
	case ERR_NOT_YOUR_TURN:         return "it is not your turn";
	case ERR_SERVER_FULL:           return "the server is full";
	case ERR_BAD_PLAYER_ID:         return "invalid player ID (use 1-15 printable ASCII characters)";
	case ERR_UNKNOWN_TYPE:          return "unknown message type";
	case ERR_DUPLICATE_PLAYER_ID:   return "that player ID is already taken";
	case ERR_ZERO_TIME:             return "received a turn with no time left";
	case ERR_MOVE_BOUNDS:           return "square is off the board";
	case ERR_MOVE_NOT_OWN:          return "no piece of yours on that square";
	case ERR_MOVE_PATTERN:          return "that piece cannot move there";
	case ERR_MOVE_SELF_CHECK:       return "move would leave your king in check";
	case ERR_MOVE_GAME_OVER:        return "the game is over";
	case ERR_MOVE_PIECE_TYPE:       return "wrong piece type (add q, r, b or n to promote, e.g. e7e8q)";
	case ERR_CONFIG_NO_KING:        return "layout: a side has no king";
	case ERR_CONFIG_SIZE:           return "layout: zero columns or rows";
	case ERR_CONFIG_COUNT:          return "layout: too many pieces";
	case ERR_CONFIG_PIECE_TYPE:     return "layout: unknown piece type";
	case ERR_CONFIG_PLACEMENT:      return "layout: piece off the board or on an occupied square";
	case ERR_CONFIG_MULTIPLE_KINGS: return "layout: a side has more than one king";
	case ERR_CONFIG_PAWN_PROMOTION: return "layout: a pawn is on its promotion row";
	case ERR_CONFIG_FINISHED:       return "layout: the game is already over";
	case ERR_CONFIG_IN_CHECK:       return "layout: a side starts in check";
	case ERR_CONFIG_COLOR:          return "layout: invalid colour";
	case ERR_UPDATE_BAD_STATE:      return "boards out of sync";
	}
	return "unknown error";
}
