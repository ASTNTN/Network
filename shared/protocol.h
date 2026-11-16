/*
 * Application protocol definitions shared by client and server.
 * Transcribed from docs/protocol_blueprint.md.
 *
 * Header-only message structs use an empty struct, which is a GNU C
 * extension (sizeof == 0). Build with -std=gnu11, not -pedantic.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

typedef unsigned char u8;
typedef signed char i8;

#define ID_MAX_SIZE 16
#define ID_MAX_LENGTH (ID_MAX_SIZE - 1)

enum Color { // Side, stored in the high bit of a placement's piece byte
	WHITE,
	BLACK
};

#define PIECE_COLOR_BIT 0x80

enum PieceType { // Move.piece / Update.piece values, and the low 7 bits of Placement.piece
	PIECE_NONE,
	PAWN,
	KNIGHT,
	BISHOP,
	ROOK,
	QUEEN,
	KING,
	PIECE_TYPE_COUNT
};

enum {
	MSG_KEEPALIVE,
	MSG_ERROR,
	MSG_CONNECT,
	MSG_ACCEPT,
	MSG_START,
	MSG_GO,
	MSG_MOVE,
	MSG_UPDATE,
	MSG_END,
	MSG_DISCONNECT,
	MSG_DRAW,
	MSG_COUNT
};

struct Header { // Message header
	u8 type;
};

// Empty structure - message is header-only
struct Keepalive { // Keepalive, bidirectional
};

struct Error { // Error code, bidirectional
	u8 code;
};

struct Connect { // Request connection, client -> server
	u8 id[ID_MAX_SIZE]; // Player ID as NUL-terminated string
};

/*
 * Whether a CONNECT ID is valid: 1 to ID_MAX_LENGTH printable ASCII
 * characters (0x20-0x7E, space included), then a NUL. Bytes after the
 * NUL are ignored.
 */
static inline int player_id_valid(const u8 id[ID_MAX_SIZE])
{
	for (int i = 0; i < ID_MAX_SIZE; i++) {
		if (id[i] == 0)
			return i > 0;
		if (id[i] < 0x20 || id[i] > 0x7E)
			return 0;
	}
	return 0; // No NUL terminator
}

// Empty structure - message is header-only
struct Accept { // Accept connection, server -> client
};

struct Placement { // One piece in the starting layout
	u8 piece; // PIECE_* type, with PIECE_COLOR_BIT set for black
	u8 row;   // 0-indexed; the same axis as Move's y
	u8 col;   // 0-indexed; the same axis as Move's x
};

/*
 * START is variable-sized: a fixed prefix (opponent ID, the receiver's
 * colour, columns, rows, piece count) followed by `count` placements. The
 * whole payload is at most START_MAX_SIZE bytes.
 */
#define START_MAX_SIZE    255
#define START_PREFIX_SIZE (ID_MAX_SIZE + 4)                          // 20
#define START_MAX_PIECES  ((START_MAX_SIZE - START_PREFIX_SIZE) / 3) // 78

struct Start { // Start match, server -> client
	u8 id[ID_MAX_SIZE]; // Opponent ID as NUL-terminated string
	u8 color; // The receiver's colour: WHITE or BLACK
	u8 cols;  // Board width
	u8 rows;  // Board height
	u8 count; // Number of placements that follow
	struct Placement pieces[START_MAX_PIECES]; // Only the first `count` are sent
};

/*
 * Two-byte numbers (GO's time, DRAW's turn) travel as two octets, most
 * significant first, so byte order never depends on the host.
 */
static inline unsigned be16_get(const u8 b[2])
{
	return (unsigned)b[0] << 8 | b[1];
}

static inline void be16_set(u8 b[2], unsigned v)
{
	b[0] = (u8)(v >> 8);
	b[1] = (u8)v;
}

/*
 * A player's remaining clock time, in whole seconds. The server never
 * sends 0; clients accept any nonzero value and answer 0 with
 * ERR_ZERO_TIME.
 */
#define CLOCK_DEFAULT_SECONDS 600 // The server's starting clock; not configurable yet
#define CLOCK_MAX_SECONDS     65535

struct Go { // Signal start of turn to active player, server -> client
	u8 time[2]; // Receiver's remaining clock time in seconds, big-endian
};

struct Move { // Move request from client to server
	u8 start_x;
	u8 start_y;
	u8 end_x;
	u8 end_y;
	u8 piece; // PIECE_* type on the end square after the move (differs from the mover only on promotion)
};

struct Update { // State update from server to clients
	u8 start_x;
	u8 start_y;
	u8 end_x;
	u8 end_y;
	u8 piece; // PIECE_* type on the end square after the move (differs from the mover only on promotion)
};

struct End { // End of game, server -> client
	i8 victory;   // Whether the receiving client won (1), lost (-1), or tied (0)
	u8 points[2]; // Receiver's material points, big-endian; doesn't affect victory
};

// Empty structure - message is header-only
struct Disconnect { // Disconnect, bidirectional
};

/*
 * Turns are numbered from 0 at START and advance with every accepted
 * move (every UPDATE), modulo 65536. An offer stands only for the turn
 * it names; offers naming any other turn are dropped.
 */
struct Draw { // Draw offer or acceptance, bidirectional
	u8 turn[2]; // Turn the offer is for, big-endian
};

/*
 * Error.code values. The high nibble is the family, the low nibble the
 * code within it. 0x00-0x0F is reserved, so 0 is never a valid code.
 */
#define ERR_FAMILY(code) ((u8)(code) >> 4)

enum {
	ERR_FAMILY_PROTOCOL = 0x1,
	ERR_FAMILY_MOVE     = 0x2,
	ERR_FAMILY_CONFIG   = 0x3,
	ERR_FAMILY_UPDATE   = 0x4
};

enum {
	// Protocol family
	ERR_UNEXPECTED_MESSAGE = 0x10, // Message not valid in the receiver's current state
	ERR_NOT_YOUR_TURN      = 0x11, // MOVE from the inactive player
	ERR_SERVER_FULL        = 0x12, // Connection refused: both player slots are taken
	ERR_BAD_PLAYER_ID      = 0x13, // CONNECT ID is empty, unterminated, or not printable ASCII
	ERR_UNKNOWN_TYPE       = 0x14, // Header type byte >= MSG_COUNT (fatal: DISCONNECT follows)
	ERR_DUPLICATE_PLAYER_ID = 0x15, // CONNECT carried the other player's ID
	ERR_ZERO_TIME          = 0x16, // GO carried a remaining time of 0

	// Move family
	ERR_MOVE_BOUNDS        = 0x20, // A coordinate is off the board
	ERR_MOVE_NOT_OWN       = 0x21, // Start square is empty or holds the opponent's piece
	ERR_MOVE_PATTERN       = 0x22, // The piece cannot move that way
	ERR_MOVE_SELF_CHECK    = 0x23, // The move would leave the mover's king in check
	ERR_MOVE_GAME_OVER     = 0x24, // The game has already ended
	ERR_MOVE_PIECE_TYPE    = 0x25, // Piece type doesn't match the mover, or isn't a valid promotion

	// Configuration family: a client rejects the START layout (fatal: DISCONNECT follows)
	ERR_CONFIG_NO_KING     = 0x30, // A side has no king
	ERR_CONFIG_SIZE        = 0x31, // Zero columns or rows
	ERR_CONFIG_COUNT       = 0x32, // More than START_MAX_PIECES placements
	ERR_CONFIG_PIECE_TYPE  = 0x33, // Unknown piece type in a placement
	ERR_CONFIG_PLACEMENT   = 0x34, // A piece is off the board or shares a square
	ERR_CONFIG_MULTIPLE_KINGS = 0x35, // A side has more than one king
	ERR_CONFIG_PAWN_PROMOTION = 0x36, // A pawn already stands on its promotion row
	ERR_CONFIG_FINISHED    = 0x37, // The position is already checkmate, stalemate or dead
	ERR_CONFIG_IN_CHECK    = 0x38, // A side starts in check
	ERR_CONFIG_COLOR       = 0x39, // The receiver's colour is neither WHITE nor BLACK

	// Update family: a client can't apply an UPDATE (fatal: DISCONNECT follows)
	ERR_UPDATE_BAD_STATE   = 0x40  // The local engine rejected the move, so the boards differ
};

enum { // End.victory values
	END_LOST = -1,
	END_TIED = 0,
	END_WON  = 1
};

static const u8 PAYLOAD_SIZE[MSG_COUNT] = {
	[MSG_KEEPALIVE]  = sizeof(struct Keepalive),
	[MSG_ERROR]      = sizeof(struct Error),
	[MSG_CONNECT]    = sizeof(struct Connect),
	[MSG_ACCEPT]     = sizeof(struct Accept),
	[MSG_START]      = START_PREFIX_SIZE, // Fixed prefix only; see payload_extra()
	[MSG_GO]         = sizeof(struct Go),
	[MSG_MOVE]       = sizeof(struct Move),
	[MSG_UPDATE]     = sizeof(struct Update),
	[MSG_END]        = sizeof(struct End),
	[MSG_DISCONNECT] = sizeof(struct Disconnect),
	[MSG_DRAW]       = sizeof(struct Draw)
};

/*
 * Bytes that follow the fixed payload, given the PAYLOAD_SIZE[type] bytes
 * already read. Returns -1 if the prefix is invalid (too many placements).
 */
static inline int payload_extra(u8 type, const u8 *payload)
{
	if (type != MSG_START)
		return 0;
	u8 count = payload[offsetof(struct Start, count)];
	return count > START_MAX_PIECES ? -1 : 3 * count;
}

// Size of a START payload carrying `count` placements.
static inline unsigned start_payload_size(u8 count)
{
	return START_PREFIX_SIZE + 3u * count;
}

// The wire format relies on the structs having no padding.
_Static_assert(sizeof(struct Header)     == 1, "Header must be 1 byte");
_Static_assert(sizeof(struct Keepalive)  == 0, "Keepalive must be empty");
_Static_assert(sizeof(struct Error)      == 1, "Error must be 1 byte");
_Static_assert(sizeof(struct Connect)    == ID_MAX_SIZE, "Connect must be unpadded");
_Static_assert(sizeof(struct Placement)  == 3, "Placement must be 3 bytes");
_Static_assert(offsetof(struct Start, pieces) == START_PREFIX_SIZE, "Start prefix must be unpadded");
_Static_assert(sizeof(struct Start) <= START_MAX_SIZE, "Start must fit the maximum payload");
_Static_assert(sizeof(struct Go)         == 2, "Go must be 2 bytes");
_Static_assert(sizeof(struct Move)       == 5, "Move must be 5 bytes");
_Static_assert(sizeof(struct Update)     == 5, "Update must be 5 bytes");
_Static_assert(sizeof(struct End)        == 3, "End must be 3 bytes");
_Static_assert(sizeof(struct Draw)       == 2, "Draw must be 2 bytes");

#endif
