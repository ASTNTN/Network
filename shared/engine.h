/*
 * Chess rules engine shared by client and server.
 *
 * The server uses game_move() to validate and classify each MOVE (the
 * FSM's CLASSIFY state). Clients use the same call to apply each UPDATE
 * to their local copy, so both sides run identical rules.
 *
 * Boards are arbitrary sizes up to 255x255, set up from a START layout.
 * Coordinates are 0-indexed: x is the column (file a = 0), y is the row
 * (rank 1 = 0). White moves toward increasing y and moves first.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdio.h>
#include "protocol.h"

#define BOARD_MAX     255              // Largest width or height
#define MAX_PIECES    START_MAX_PIECES // Captures only ever remove pieces
#define MOVE_TEXT_MAX 12               // Longest move text ("iu255iu254q") plus NUL

// Pieces are one byte: type | color << 7, the same as Placement.piece.
#define SQ_EMPTY 0
#define SQ_MAKE(type, color) ((u8)((type) | ((color) << 7)))
#define SQ_TYPE(sq)  ((u8)((sq) & 0x7F))
#define SQ_COLOR(sq) ((u8)((sq) >> 7))

enum GameStatus {
	STATUS_ONGOING,
	STATUS_WON,   // `winner` holds the winning color
	STATUS_DRAWN
};

struct PieceState {
	u8 code;  // type | color << 7
	u8 x, y;
	u8 moved; // Whether the piece has moved (pawn double step, castling)
};

struct Game {
	u8 width, height;
	u8 npieces;
	struct PieceState pieces[MAX_PIECES]; // In no particular order
	u8 turn;     // Color to move
	unsigned ply; // Moves applied since the start; DRAW's turn number is ply % 65536
	unsigned points[2]; // Material each colour has captured (see piece_value()); doesn't affect victory
	u8 ep_valid; // Whether (ep_x, ep_y) is an en passant target square
	u8 ep_x;
	u8 ep_y;
	u8 status;   // STATUS_*
	u8 winner;   // Color, valid when status == STATUS_WON
};

// Standard chess starting position, for the server to build a START from.
extern const char *const STANDARD_FEN;

// Result of game_move(), matching the outcomes of the FSM's CLASSIFY state.
enum MoveResult {
	MOVE_CONTINUE,          // Valid, non-terminal move
	MOVE_VICTORY,           // Valid move; the mover has won
	MOVE_DRAW,              // Valid move; the game is drawn
	// Everything below is invalid and leaves the game unchanged.
	MOVE_INVALID_GAME_OVER, // The game has already ended
	MOVE_INVALID_BOUNDS,    // A coordinate is off the board
	MOVE_INVALID_NOT_OWN,   // Start square is empty or holds an opponent's piece
	MOVE_INVALID_PATTERN,   // The piece cannot move that way
	MOVE_INVALID_SELF_CHECK,// The move would leave the mover's king in check
	MOVE_INVALID_PIECE_TYPE // `piece` doesn't match the mover, or isn't a valid promotion
};

#define MOVE_IS_INVALID(r) ((r) >= MOVE_INVALID_GAME_OVER)

// Set up a game from a START layout. White moves first and no piece has
// moved yet. Returns 0, or the ERR_CONFIG_* code to send if the layout is
// invalid. The receiver's colour (s->color) is only validated; the caller
// keeps it. Checks run in this order: size, count, colour; then for each placement,
// type, square, and a pawn on its promotion row; then each side has
// exactly one king; then the position isn't already finished; then
// neither side is in check.
u8 game_from_start(struct Game *g, const struct Start *s);

// Fill in the layout fields of `s` (not `id` or `color`) from the current position.
// Returns the START payload size to send.
unsigned game_to_start(const struct Game *g, struct Start *s);

// Load a position from FEN (piece placement, side, castling, en passant;
// the move counters are ignored). Any board size up to BOARD_MAX is
// accepted, with multi-digit empty counts. Castling rights K/Q/k/q refer
// to the rooks in the corners. Returns 0, or -1 if malformed.
int game_load_fen(struct Game *g, const char *fen);

// Material value of a piece type: pawn 1, knight 3, bishop 3, rook 5,
// queen 9, king 0. Capturing a piece scores its value for the capturer.
unsigned piece_value(u8 type);

// The piece on (x, y), or SQ_EMPTY.
u8 game_piece_at(const struct Game *g, int x, int y);

// Validate a move for the side to move, and apply it if valid.
// Invalid moves leave `g` untouched. `m->piece` must equal the moving
// piece's type, except on promotion, where it names the new piece.
// struct Update has the same layout; clients copy its fields into a struct Move.
enum MoveResult game_move(struct Game *g, const struct Move *m);

// Write up to `max` legal moves into `out`, and return the total number of
// legal moves. Each promotion choice is a separate move.
int game_legal_moves(const struct Game *g, struct Move *out, int max);

// Whether `color`'s king is attacked.
int game_in_check(const struct Game *g, u8 color);

// Print the board, with `perspective`'s side at the bottom.
void game_print(const struct Game *g, FILE *f, u8 perspective);

// Parse a move like "e2e4", "e2 e4", "e2-e4", "aa10ab12", or "e7e8q" for a
// promotion. Columns past z are aa, ab, ... Without a promotion letter,
// `piece` is set to PIECE_NONE; fill it in with game_fill_piece() before
// sending. Returns 0, or -1 if malformed.
int move_parse(const char *s, struct Move *out);

// If `m->piece` is PIECE_NONE, set it to the type of the piece on the start square.
void game_fill_piece(const struct Game *g, struct Move *m);

// Format a move into `buf` (at least MOVE_TEXT_MAX bytes), adding a
// promotion letter when `piece` differs from the piece on the start square
// in `g`. Call this before applying the move.
void move_format(const struct Game *g, const struct Move *m, char *buf);

// The ERR_MOVE_* code to send for an invalid result, or 0 for a valid one.
u8 move_error_code(enum MoveResult r);

// Human-readable description of a move result.
const char *move_result_str(enum MoveResult r);

#endif
