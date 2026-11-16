# Application Protocol

## 2.1 Transport Layer & Packet Framing Mechanism

Each device keeps its state entirely local. Only changes in state are transmitted, and the server verifies all state changes.

- **Game**: (Fairy) chess
- **Language**: C
- **Transport Protocol**: TCP

### Framing Rule

Messages of a specific type have a predetermined size in bytes, except `START`, whose fixed prefix says how many bytes follow it.

Reading messages takes up to three stages.

1. The message header byte is read from the TCP stream, identifying the message payload to follow. 
2. The message payload length is derived from the header type by lookup table, and the message payload is read from the stream.
3. For `START` only, `payload_extra()` reads the placement count from the prefix just read, and the remaining `count × 3` bytes of placements are read from the stream.

In the event of a partial read, the reader must continue reading from the TCP stream until a full message has been accumulated. If the message is not completed within `MESSAGE_TIMEOUT_MS` (ten seconds) of its first byte arriving, the connection is considered dead (see Timing).

Multiple messages can be inbound in the TCP stream, but only one message will be read out from that stream at a time.

### Serialization Format

The message serialization format is a custom binary schema.

All messages are constructed from sequential reads of the following `struct Header` + payload pairs without padding. Because individual fields are constructed from octets, endianness is avoided entirely.

Header-only messages are declared as empty structures, which is a GNU C extension (`sizeof` is 0), so the code is compiled with `-std=gnu11`.

All message formats share a type header, but possess different fields.

```c
#include <stddef.h> // offsetof

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
	PIECE_NONE, // Never valid on the wire
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
	u8 cols;  // Board width, 1-255
	u8 rows;  // Board height, 1-255
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
	u8 piece; // PIECE_* type on the end square after the move
};

struct Update { // State update from server to clients
	u8 start_x;
	u8 start_y;
	u8 end_x;
	u8 end_y;
	u8 piece; // PIECE_* type on the end square after the move
};

struct End { // End of game, server -> client
	i8 victory;   // As an enum. Whether the receiving client won (1), lost (-1), or tied (0)
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

// static, so each file including the header gets its own copy instead of a duplicate symbol
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
```

### Examples

#### Connection Message

```
|MSG_CONNECT|'X'|'a'|'v'|'i'|'e'|'r'|0|0|0|0|0|0|0|0|0|0|
```

The connection message consists of a header with a type of `MSG_CONNECT`, followed by a 16-byte NUL-terminated string containing the player's ID "Xavier" (with a maximum length of fifteen non-NUL characters).

#### Acceptance Message

```
|MSG_ACCEPT|
```

The acceptance message consists of a header with a type of `MSG_ACCEPT`.

#### Start Message

```
|MSG_START|'B'|'o'|'b'|0|0|0|0|0|0|0|0|0|0|0|0|0|WHITE|8|8|32|ROOK|0|0|KNIGHT|0|1| ... |KING+0x80|7|4| ... |
```

The start message for the standard position, sent to a player who plays white against "Bob", consists of a header with a type of `MSG_START`, the 16-byte opponent ID, the receiver's colour (`WHITE` = 0), 8 columns, 8 rows and 32 placements, then 32 (piece, row, column) trios. The white rook on a1 is `ROOK, 0, 0`, and the black king on e8 is `KING | PIECE_COLOR_BIT, 7, 4`. The payload is 20 + 32 × 3 = 116 bytes. Bob's `START` is the same except for the opponent ID and `BLACK` = 1.

#### End Message

```
|MSG_END|1|0x00|0x0D|
```

The end message consists of a header with a type of `MSG_END`, the result for the receiver (won = 1), and the receiver's material points as two bytes, most significant first: here 13 points, for example a rook, a bishop, a knight and two pawns captured.

#### Go Message

```
|MSG_GO|0x02|0x58|
```

The go message consists of a header with a type of `MSG_GO`, followed by the receiver's remaining clock time in seconds as two bytes, most significant first. `0x0258` is 600 seconds, the full ten minutes.

#### Draw Message

```
|MSG_DRAW|0x00|0x0C|
```

The draw message consists of a header with a type of `MSG_DRAW`, followed by the turn the offer is for as two bytes, most significant first. `0x000C` is turn 12: twelve moves have been made, so it is white's seventh move.

#### Move Message

```
|MSG_MOVE|4|1|4|3|PAWN|
```

A move from e2 to e4 consists of a header with a type of `MSG_MOVE`, followed by the start and end coordinates (0-indexed; `x` is the file with a = 0, `y` is the rank with rank 1 = 0) and the piece type.

### Piece Types

The `piece` field of `MOVE` and `UPDATE` holds the type of the piece that stands on the end square after the move:

- For an ordinary move, it must equal the type of the moving piece.
- For a promotion, it names the new piece: `QUEEN`, `ROOK`, `BISHOP` or `KNIGHT`.

Any other value is rejected with `ERR_MOVE_PIECE_TYPE`.

| Value | Type |
|:-----:|:-----|
| 0 | `PIECE_NONE` (never valid on the wire) |
| 1 | `PAWN` |
| 2 | `KNIGHT` |
| 3 | `BISHOP` |
| 4 | `ROOK` |
| 5 | `QUEEN` |
| 6 | `KING` |

Fairy pieces are appended after `KING`.

### Castling

A castle is sent as a single `MOVE` of the king two squares toward the rook, with `piece` = `KING` (for example `|MSG_MOVE|4|0|6|0|KING|` for white's kingside castle, e1 to g1). A king can't move two squares any other way, so this move always means castling. The rook isn't sent: the receiver moves it to the square the king passed over, onto the far side of the king. The server broadcasts the same move in an `UPDATE`, and each client moves the rook the same way.

### Board Layout

`START` describes the starting position itself, so any layout up to 255 × 255 squares and 78 pieces can be played:

- `color` tells the receiver which side it plays. The server gives white to the player it picks at random to move first.
- `cols` and `rows` give the board size. Both must be at least 1.
- Each placement puts one piece on the board. The low 7 bits of `piece` are its type (see Piece Types), and the high bit is its colour (clear for white, set for black). `row` and `col` are 0-indexed and use the same axes as `MOVE`'s `y` and `x`.
- White moves toward increasing rows and moves first. No piece in a `START` layout has moved yet, which allows castling and pawn double steps.

The server currently sends only the standard chess position. Clients must still accept any valid layout. A client checks the layout in this order and reports the first problem it finds:

1. `ERR_CONFIG_SIZE`: zero columns or rows.
2. `ERR_CONFIG_COUNT`: more than 78 placements. The client checks this as soon as it has read the 20-byte prefix, and doesn't read the placements.
3. `ERR_CONFIG_COLOR`: `color` is neither `WHITE` (0) nor `BLACK` (1).
4. For each placement in order:
	- `ERR_CONFIG_PIECE_TYPE`: an unknown type.
	- `ERR_CONFIG_PLACEMENT`: a square off the board or already occupied.
	- `ERR_CONFIG_PAWN_PROMOTION`: a pawn on its promotion row (the last row for white, the first row for black).
5. `ERR_CONFIG_NO_KING`: a side has no king.
6. `ERR_CONFIG_MULTIPLE_KINGS`: a side has more than one king.
7. `ERR_CONFIG_FINISHED`: the game is already over, because white (to move) is checkmated or stalemated or the position is dead.
8. `ERR_CONFIG_IN_CHECK`: either side starts in check.

An invalid layout is fatal (see Fatal Errors): the client sends `ERROR` with the code, then `DISCONNECT`, and closes the connection. When the server receives a Configuration-family `ERROR` during gameplay, it ends the game without a winner. Both clients that are still connected receive `END` with tied (`0`).

### Score

`END` reports the result (won, lost or tied) and the receiver's material points. Points are a score summary only and never affect the result. A player scores the value of each opponent piece they capture:

| Piece | Points |
|:------|:------:|
| Pawn | 1 |
| Knight | 3 |
| Bishop | 3 |
| Rook | 5 |
| Queen | 9 |
| King | 0 |

En passant scores the pawn. A promoted piece is worth its new type when captured, and promoting scores nothing by itself. The field is two bytes, most significant first. Each client also tracks both players' points with its own engine, so it can show them during the game.

### Draw Offers

A client may send `DRAW` at any time, and no peer ever answers `DRAW` with `ERR_UNEXPECTED_MESSAGE`. Each `DRAW` names the turn it is for. Turns are numbered from 0 at `START`, and the number advances with every accepted move, so it equals the number of `UPDATE`s so far, modulo 65,536. Both players offer and accept draws on either player's turn.

- An offer stands only for the turn it was made on. A `DRAW` that names any other turn (an old offer that crossed with a move, or a turn that hasn't happened yet) is dropped without a reply, by the server and by clients.
- During gameplay (the server's `TURN` state), when the server receives a `DRAW` for the current turn from a client whose opponent hasn't offered on that turn, it records the offer and sends the same `DRAW` to the opponent. A repeat offer from the same client on the same turn is dropped.
- When the server has received a `DRAW` for the current turn from both clients, the game ends in a draw: each client receives `END` with `victory` = tied (`0`).
- When a move is accepted, the turn advances, and any offer from the old turn lapses. No message announces the lapse; the `UPDATE` makes it clear to both clients.

Outside gameplay, the server ignores `DRAW`. Offering a draw doesn't use up a turn: a player whose turn it is must still send a `MOVE`.

### Clocks

Each player has a clock holding their remaining time in whole seconds. The server keeps both clocks and is the only authority on them.

- Both clocks start at `CLOCK_DEFAULT_SECONDS` (ten minutes). The value can't be configured yet.
- A player's clock runs only while they are `ACTIVE`, from the moment their `GO` is sent until their valid `MOVE` arrives. With one-second granularity, network delay is ignored.
- Every turn costs the whole seconds elapsed, and at least one second.
- Each `GO` carries the receiver's remaining time, including a retry `GO` after an invalid move.
- A player whose clock reaches zero loses, and the opponent wins. This holds even when the opponent couldn't checkmate; that may be refined in the future.

The time field is two bytes, so a clock can hold up to 65,535 seconds. The server never sends a `GO` with zero time. Clients must accept any nonzero value, not just ten minutes. A client that receives a `GO` with zero time replies with `ERROR` (`ERR_ZERO_TIME`) and doesn't prompt for a move.

### Timing

All timing values are in `shared/timing.h`, kept together so they can be tuned:

| Constant | Value | Meaning |
|:---------|:-----:|:--------|
| `KEEPALIVE_INTERVAL_MS` | 1,000 (1 s) | How often each peer sends `KEEPALIVE` |
| `KEEPALIVE_TIMEOUT_MS` | 60,000 (1 min) | A peer that has sent nothing for this long is considered dead |
| `MESSAGE_TIMEOUT_MS` | 10,000 (10 s) | Once a message's first byte arrives, the rest must arrive within this time |
| `POLL_MS` | 50 | Longest wait in `poll()` before a thread checks its other duties |

The keepalive timeout also limits how long an unassigned connection can keep retrying `CONNECT` without sending anything.

### Connecting

- **Player IDs:** an ID must be 1 to 15 printable ASCII characters (`0x20`–`0x7E`, space included) followed by a NUL; bytes after the NUL are ignored (`player_id_valid()`). The server replies to `CONNECT` with `ERROR` (`ERR_BAD_PLAYER_ID`) if the ID is empty, has no NUL terminator, or contains any other byte. It replies with `ERROR` (`ERR_DUPLICATE_PLAYER_ID`) if the ID matches the other player's. In both cases the connection stays open until its keepalive timeout, so the client can send `CONNECT` again with a different ID.
- **Full server:** when both player slots are taken, the main thread accepts the extra connection, sends it `ERROR` (`ERR_SERVER_FULL`) and `DISCONNECT`, and closes the socket. Beyond three simultaneous connections, the server may drop further ones without sending anything.

### Error Codes

`ERROR.code` values are grouped into families. The high nibble is the family and the low nibble is the code within it (`family = code >> 4`). `0x00`–`0x0F` is reserved, so `0` is never a valid code.

```c
enum {
	// Protocol family (0x1_)
	ERR_UNEXPECTED_MESSAGE  = 0x10, // Message not valid in the receiver's current state
	ERR_NOT_YOUR_TURN       = 0x11, // MOVE from the inactive player
	ERR_SERVER_FULL         = 0x12, // Connection refused: both player slots are taken
	ERR_BAD_PLAYER_ID       = 0x13, // CONNECT ID is empty, unterminated, or not printable ASCII
	ERR_UNKNOWN_TYPE        = 0x14, // Header type byte >= MSG_COUNT (fatal)
	ERR_DUPLICATE_PLAYER_ID = 0x15, // CONNECT carried the other player's ID
	ERR_ZERO_TIME           = 0x16, // GO carried a remaining time of 0

	// Move family (0x2_)
	ERR_MOVE_BOUNDS        = 0x20, // A coordinate is off the board
	ERR_MOVE_NOT_OWN       = 0x21, // Start square is empty or holds the opponent's piece
	ERR_MOVE_PATTERN       = 0x22, // The piece cannot move that way
	ERR_MOVE_SELF_CHECK    = 0x23, // The move would leave the mover's king in check
	ERR_MOVE_GAME_OVER     = 0x24, // The game has already ended
	ERR_MOVE_PIECE_TYPE    = 0x25, // piece doesn't match the mover, or isn't a valid promotion

	// Configuration family (0x3_): a client rejects the START layout (fatal)
	ERR_CONFIG_NO_KING        = 0x30, // A side has no king
	ERR_CONFIG_SIZE           = 0x31, // Zero columns or rows
	ERR_CONFIG_COUNT          = 0x32, // More than START_MAX_PIECES placements
	ERR_CONFIG_PIECE_TYPE     = 0x33, // Unknown piece type in a placement
	ERR_CONFIG_PLACEMENT      = 0x34, // A piece is off the board or shares a square
	ERR_CONFIG_MULTIPLE_KINGS = 0x35, // A side has more than one king
	ERR_CONFIG_PAWN_PROMOTION = 0x36, // A pawn already stands on its promotion row
	ERR_CONFIG_FINISHED       = 0x37, // The position is already checkmate, stalemate or dead
	ERR_CONFIG_IN_CHECK       = 0x38, // A side starts in check
	ERR_CONFIG_COLOR          = 0x39, // The receiver's colour is neither WHITE nor BLACK

	// Update family (0x4_): a client can't apply an UPDATE (fatal)
	ERR_UPDATE_BAD_STATE   = 0x40  // The local engine rejected the move, so the boards differ
};
```

When a move has more than one problem, the server reports the first one in this order: `ERR_MOVE_GAME_OVER`, `ERR_MOVE_BOUNDS`, `ERR_MOVE_NOT_OWN`, `ERR_MOVE_PATTERN`, `ERR_MOVE_PIECE_TYPE`, `ERR_MOVE_SELF_CHECK`. In particular, a move whose squares are unreachable reports `ERR_MOVE_PATTERN` even if its `piece` is also wrong.

### Fatal Errors

Three errors end the connection. The peer that detects one sends `ERROR` with the code, then `DISCONNECT`, and closes the socket:

- **`ERR_UNKNOWN_TYPE`** (either side): the header type byte is `MSG_COUNT` or more. With no known payload length, the receiver can't find the next message.
- **Configuration family** (client): the `START` layout is invalid. The server ends the game tied.
- **`ERR_UPDATE_BAD_STATE`** (client): the client's engine rejected an `UPDATE`, so its board no longer matches the server's.

Apart from the tie after a Configuration error, the server treats the disconnect like any other: during gameplay the other player wins, and while waiting for players the slot is cleared.

## 2.2 Application Message Types

|    Type    |    Direction     | Purpose & Description                                                |
|:----------:|:----------------:|:---------------------------------------------------------------------|
| KEEPALIVE  |  bidirectional   | Peers send regular signals to confirm that the connection is live    |
|   ERROR    |  bidirectional   | Peers send errors for invalid moves, IDs, layouts and updates, and for unexpected messages |
|  CONNECT   | client -> server | Client requests to connect to server and peer in game under `id`     |
|   ACCEPT   | server -> client | Server accepts client request to connect                             |
|   START    | server -> client | Server notifies both clients that gameplay is beginning, with the opponent's ID, the receiver's colour and the board layout |
|     GO     | server -> client | Server notifies one client that their turn is beginning, with their remaining clock time |
|    MOVE    | client -> server | Client submits requested move (squares and resulting piece type) to server |
|   UPDATE   | server -> client | Server broadcasts approved updates to both clients                   |
|    END     | server -> client | Server notifies both clients that the game has concluded, whether each won, lost, or tied, and each one's material points |
| DISCONNECT |  bidirectional   | Peers send intentional termination of connection                     |
|    DRAW    |  bidirectional   | Client offers or accepts a draw for the current turn; server forwards an offer to the opponent |

Intentional and unintentional disconnects by a client result in a forfeit.
