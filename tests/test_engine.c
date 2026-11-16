#include "engine.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// Play a move given as text, e.g. "e2e4" or "e7e8q". Without a promotion
// letter, the piece type is filled in from the board, as the client does.
static enum MoveResult play(struct Game *g, const char *text)
{
	struct Move m;
	if (move_parse(text, &m) != 0) {
		fprintf(stderr, "bad move text in test: %s\n", text);
		failures++;
		return MOVE_INVALID_BOUNDS;
	}
	game_fill_piece(g, &m);
	return game_move(g, &m);
}

// Play a move with an explicit piece type.
static enum MoveResult play_as(struct Game *g, const char *text, u8 piece)
{
	struct Move m;
	CHECK(move_parse(text, &m) == 0);
	m.piece = piece;
	return game_move(g, &m);
}

#define LIST_MAX 512

static u8 at(const struct Game *g, const char *square)
{
	struct Move m;
	char text[16];
	snprintf(text, sizeof text, "%s%s", square, square); // Parse as a null move
	CHECK(move_parse(text, &m) == 0);
	return game_piece_at(g, m.start_x, m.start_y);
}

static void standard(struct Game *g)
{
	CHECK(game_load_fen(g, STANDARD_FEN) == 0);
}

static unsigned long perft(const struct Game *g, int depth)
{
	struct Move moves[LIST_MAX];
	int n = game_legal_moves(g, moves, LIST_MAX);
	CHECK(n <= LIST_MAX);
	if (depth == 1)
		return (unsigned long)n;

	unsigned long total = 0;
	for (int i = 0; i < n; i++) {
		struct Game next = *g;
		enum MoveResult r = game_move(&next, &moves[i]);
		CHECK(!MOVE_IS_INVALID(r));
		total += perft(&next, depth - 1);
	}
	return total;
}

static void check_perft(const char *name, const char *fen, const unsigned long *expected, int depths)
{
	struct Game g;
	CHECK(game_load_fen(&g, fen) == 0);
	for (int d = 1; d <= depths; d++) {
		unsigned long got = perft(&g, d);
		if (got != expected[d - 1]) {
			fprintf(stderr, "perft %s depth %d: got %lu, expected %lu\n", name, d, got, expected[d - 1]);
			failures++;
		}
	}
}

// Reference counts from https://www.chessprogramming.org/Perft_Results.
static void test_perft(void)
{
	static const unsigned long start[] = {20, 400, 8902, 197281};
	check_perft("start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", start, 4);

	static const unsigned long kiwipete[] = {48, 2039, 97862};
	check_perft("kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", kiwipete, 3);

	static const unsigned long pos3[] = {14, 191, 2812, 43238};
	check_perft("position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", pos3, 4);

	// Positions 4 and 5 include promotions, each choice counted separately.
	static const unsigned long pos4[] = {6, 264, 9467};
	check_perft("position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", pos4, 3);

	static const unsigned long pos5[] = {44, 1486, 62379};
	check_perft("position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", pos5, 3);
}

static void test_start_layout(void)
{
	struct Game g, h;
	struct Start start = {0};

	// Round trip: standard position -> START -> game
	standard(&g);
	CHECK(game_to_start(&g, &start) == START_PREFIX_SIZE + 32 * 3);
	CHECK(start.cols == 8 && start.rows == 8 && start.count == 32);
	CHECK(game_from_start(&h, &start) == 0);
	CHECK(h.turn == WHITE && h.status == STATUS_ONGOING);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			CHECK(game_piece_at(&g, x, y) == game_piece_at(&h, x, y));
	CHECK(at(&h, "e1") == SQ_MAKE(KING, WHITE));
	CHECK(at(&h, "d8") == SQ_MAKE(QUEEN, BLACK));
	CHECK(perft(&h, 3) == 8902);

	// Nothing in a START layout has moved, so castling is available
	CHECK(game_load_fen(&g, "r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1") == 0);
	game_to_start(&g, &start);
	CHECK(game_from_start(&h, &start) == 0);
	CHECK(play(&h, "e1g1") == MOVE_CONTINUE);

	// Placement byte layout: type | color << 7, then row, then column
	CHECK(game_load_fen(&g, "8/8/8/8/8/8/8/1k6 w - - 0 1") == 0);
	game_to_start(&g, &start);
	CHECK(start.pieces[0].piece == (KING | PIECE_COLOR_BIT));
	CHECK(start.pieces[0].row == 0 && start.pieces[0].col == 1);

	// Invalid layouts, each with its configuration error code
	struct Start bad = {.cols = 8, .rows = 8, .count = 2,
		.pieces = {{KING, 0, 0}, {KING | PIECE_COLOR_BIT, 7, 7}}};
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_FINISHED); // Bare kings: a dead position
	bad.cols = 0;
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_SIZE);
	bad.cols = 8; bad.rows = 0;
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_SIZE);
	bad.rows = 8; bad.count = START_MAX_PIECES + 1;
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_COUNT);
	bad.count = 2; bad.color = 2;
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_COLOR);
	bad.color = BLACK;
	bad.count = 2;
	bad.pieces[0] = (struct Placement){PIECE_NONE, 0, 0};
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PIECE_TYPE);
	bad.pieces[0] = (struct Placement){PIECE_TYPE_COUNT | PIECE_COLOR_BIT, 0, 0};
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PIECE_TYPE);
	bad.pieces[0] = (struct Placement){KING, 8, 0}; // Row off the board
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PLACEMENT);
	bad.pieces[0] = (struct Placement){KING, 0, 8}; // Column off the board
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PLACEMENT);
	bad.pieces[0] = (struct Placement){KING, 7, 7}; // Same square as the black king
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PLACEMENT);
	bad.pieces[0] = (struct Placement){QUEEN, 0, 0}; // White has no king
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_NO_KING);
	bad.pieces[0] = (struct Placement){KING, 0, 0};
	bad.pieces[1] = (struct Placement){QUEEN | PIECE_COLOR_BIT, 7, 7}; // Black has no king
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_NO_KING);
	bad.count = 0;
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_NO_KING);
	bad.count = 3; // Two white kings
	bad.pieces[1] = (struct Placement){KING | PIECE_COLOR_BIT, 7, 7};
	bad.pieces[2] = (struct Placement){KING, 0, 2};
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_MULTIPLE_KINGS);
	bad.pieces[2] = (struct Placement){PAWN, 7, 2}; // White pawn on row 8
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PAWN_PROMOTION);
	bad.pieces[2] = (struct Placement){PAWN | PIECE_COLOR_BIT, 0, 2}; // Black pawn on row 1
	CHECK(game_from_start(&g, &bad) == ERR_CONFIG_PAWN_PROMOTION);
	bad.pieces[2] = (struct Placement){PAWN, 0, 2}; // White pawn on its own back row is allowed
	CHECK(game_from_start(&g, &bad) == 0);
	struct Start fin = {0};
	CHECK(game_load_fen(&g, "7k/8/8/8/8/8/5PPP/r5K1 w - - 0 1") == 0); // White is checkmated
	game_to_start(&g, &fin);
	CHECK(game_from_start(&h, &fin) == ERR_CONFIG_FINISHED);
	CHECK(game_load_fen(&g, "k7/8/8/8/8/1q6/7p/K7 w - - 0 1") == 0); // White is stalemated
	game_to_start(&g, &fin);
	CHECK(game_from_start(&h, &fin) == ERR_CONFIG_FINISHED);
	CHECK(game_load_fen(&g, "4k3/8/8/8/8/8/8/K3R3 w - - 0 1") == 0); // Black, not to move, in check
	game_to_start(&g, &fin);
	CHECK(game_from_start(&h, &fin) == ERR_CONFIG_IN_CHECK);
	CHECK(game_load_fen(&g, "4r1k1/8/8/8/8/8/7P/4K3 w - - 0 1") == 0); // White in check, can escape
	game_to_start(&g, &fin);
	CHECK(game_from_start(&h, &fin) == ERR_CONFIG_IN_CHECK);
	for (u8 c = ERR_CONFIG_NO_KING; c <= ERR_CONFIG_IN_CHECK; c++)
		CHECK(ERR_FAMILY(c) == ERR_FAMILY_CONFIG);
	CHECK(ERR_FAMILY(ERR_UPDATE_BAD_STATE) == ERR_FAMILY_UPDATE);
	CHECK(ERR_FAMILY(ERR_DUPLICATE_PLAYER_ID) == ERR_FAMILY_PROTOCOL);
	CHECK(ERR_FAMILY(ERR_ZERO_TIME) == ERR_FAMILY_PROTOCOL);

	// Framing helper: bytes after the fixed prefix
	u8 prefix[START_PREFIX_SIZE] = {0};
	CHECK(START_PREFIX_SIZE == 20 && START_MAX_PIECES == 78);
	CHECK(offsetof(struct Start, color) == ID_MAX_SIZE);
	prefix[offsetof(struct Start, count)] = START_MAX_PIECES;
	CHECK(payload_extra(MSG_START, prefix) == 3 * START_MAX_PIECES);
	CHECK(START_PREFIX_SIZE + 3 * START_MAX_PIECES <= START_MAX_SIZE);
	prefix[offsetof(struct Start, count)] = START_MAX_PIECES + 1;
	CHECK(payload_extra(MSG_START, prefix) == -1);
	CHECK(payload_extra(MSG_MOVE, prefix) == 0);
}

static void test_large_boards(void)
{
	struct Game g;
	struct Move moves[LIST_MAX];
	char buf[MOVE_TEXT_MAX];

	// 255x255: white rook a1 and king a2, black king in the far corner
	struct Start start = {.cols = 255, .rows = 255, .count = 3, .pieces = {
		{ROOK, 0, 0}, {KING, 1, 0}, {KING | PIECE_COLOR_BIT, 254, 254}}};
	CHECK(game_from_start(&g, &start) == 0);
	CHECK(game_legal_moves(&g, moves, 0) == 254 + 4); // Rook along row 1, king to 4 squares
	CHECK(game_move(&g, &(struct Move){0, 0, 255, 0, ROOK}) == MOVE_INVALID_BOUNDS);
	CHECK(play(&g, "a1iu1") == MOVE_CONTINUE);
	CHECK(game_piece_at(&g, 254, 0) == SQ_MAKE(ROOK, WHITE));
	CHECK(game_in_check(&g, BLACK)); // Along column iu
	CHECK(play(&g, "iu255iu254") == MOVE_INVALID_SELF_CHECK);
	CHECK(play(&g, "iu255it254") == MOVE_CONTINUE);

	// 10x8 with kings on f: castling still moves the king two squares
	CHECK(game_load_fen(&g, "r4k3r/10/10/10/10/10/10/R4K3R w KQkq - 0 1") == 0);
	CHECK(g.width == 10 && g.height == 8);
	CHECK(play(&g, "f1h1") == MOVE_CONTINUE);
	CHECK(at(&g, "h1") == SQ_MAKE(KING, WHITE));
	CHECK(at(&g, "g1") == SQ_MAKE(ROOK, WHITE));
	CHECK(at(&g, "j1") == SQ_EMPTY);
	CHECK(play(&g, "f8d8") == MOVE_CONTINUE);
	CHECK(at(&g, "d8") == SQ_MAKE(KING, BLACK));
	CHECK(at(&g, "e8") == SQ_MAKE(ROOK, BLACK));

	// An unmoved pawn may double step from any row; it promotes on arrival
	start = (struct Start){.cols = 3, .rows = 4, .count = 3, .pieces = {
		{PAWN, 1, 0}, {KING, 0, 2}, {KING | PIECE_COLOR_BIT, 3, 2}}};
	CHECK(game_from_start(&g, &start) == 0);
	CHECK(play_as(&g, "a2a4", ROOK) == MOVE_CONTINUE);
	CHECK(game_piece_at(&g, 0, 3) == SQ_MAKE(ROOK, WHITE));

	// Coordinates past z
	struct Move m;
	CHECK(move_parse("aa10ab12", &m) == 0);
	CHECK(m.start_x == 26 && m.start_y == 9 && m.end_x == 27 && m.end_y == 11);
	CHECK(move_parse("iu255a1", &m) == 0 && m.start_x == 254 && m.start_y == 254);
	CHECK(move_parse("iv1a1", &m) == -1);  // Column 256
	CHECK(move_parse("a256a1", &m) == -1); // Row 256
	CHECK(move_parse("a0a1", &m) == -1);
	CHECK(game_from_start(&g, &(struct Start){.cols = 255, .rows = 255, .count = 3,
		.pieces = {{PAWN, 253, 254}, {KING, 0, 0}, {KING | PIECE_COLOR_BIT, 0, 2}}}) == 0);
	m = (struct Move){254, 253, 254, 254, QUEEN};
	move_format(&g, &m, buf);
	CHECK(strcmp(buf, "iu254iu255q") == 0);
}

static void test_invalid_moves(void)
{
	struct Game g, before;
	standard(&g);
	before = g;

	CHECK(game_move(&g, &(struct Move){4, 1, 4, 8, PAWN}) == MOVE_INVALID_BOUNDS);
	CHECK(play(&g, "e3e4") == MOVE_INVALID_NOT_OWN);  // Empty square
	CHECK(play(&g, "e7e5") == MOVE_INVALID_NOT_OWN);  // Opponent's piece
	CHECK(play(&g, "e2e5") == MOVE_INVALID_PATTERN);
	CHECK(play(&g, "f1c4") == MOVE_INVALID_PATTERN);  // Blocked
	CHECK(memcmp(&g, &before, sizeof g) == 0);       // Untouched

	// Pinned bishop
	CHECK(game_load_fen(&g, "4k3/4r3/8/8/8/8/4B3/4K3 w - - 0 1") == 0);
	CHECK(play(&g, "e2d3") == MOVE_INVALID_SELF_CHECK);
}

static void test_checkmate(void)
{
	struct Game g;
	standard(&g);
	CHECK(play(&g, "f2f3") == MOVE_CONTINUE);
	CHECK(play(&g, "e7e5") == MOVE_CONTINUE);
	CHECK(play(&g, "g2g4") == MOVE_CONTINUE);
	CHECK(play(&g, "d8h4") == MOVE_VICTORY);
	CHECK(g.status == STATUS_WON);
	CHECK(g.winner == BLACK);
	CHECK(play(&g, "a2a3") == MOVE_INVALID_GAME_OVER);
}

static void test_draws(void)
{
	struct Game g;

	// Stalemate
	CHECK(game_load_fen(&g, "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1") == 0);
	CHECK(play(&g, "f1f7") == MOVE_DRAW);
	CHECK(g.status == STATUS_DRAWN);

	// King captures the last pawn: K v K
	CHECK(game_load_fen(&g, "4k3/8/8/8/8/8/3p4/4K3 w - - 0 1") == 0);
	CHECK(play(&g, "e1d2") == MOVE_DRAW);

	// K + B v K + B with bishops on the same square color
	CHECK(game_load_fen(&g, "4k3/8/8/8/8/8/1b6/K3B3 w - - 0 1") == 0);
	CHECK(play(&g, "a1b1") == MOVE_DRAW);

	// K + B v K + B with bishops on different square colors is not dead
	CHECK(game_load_fen(&g, "4k3/8/8/8/8/8/2b5/K3B3 b - - 0 1") == 0);
	CHECK(play(&g, "e8d8") == MOVE_CONTINUE);
}

static void test_points(void)
{
	struct Game g;

	CHECK(piece_value(PAWN) == 1 && piece_value(KNIGHT) == 3 && piece_value(BISHOP) == 3);
	CHECK(piece_value(ROOK) == 5 && piece_value(QUEEN) == 9 && piece_value(KING) == 0);
	CHECK(piece_value(PIECE_NONE) == 0 && piece_value(PIECE_TYPE_COUNT) == 0);

	// Ordinary captures score the captured piece's value for the capturer
	standard(&g);
	CHECK(g.points[WHITE] == 0 && g.points[BLACK] == 0);
	CHECK(play(&g, "e2e4") == MOVE_CONTINUE);
	CHECK(play(&g, "d7d5") == MOVE_CONTINUE);
	CHECK(play(&g, "e4d5") == MOVE_CONTINUE); // Pawn takes pawn
	CHECK(g.points[WHITE] == 1 && g.points[BLACK] == 0);
	CHECK(play(&g, "d8d5") == MOVE_CONTINUE); // Queen takes pawn
	CHECK(g.points[WHITE] == 1 && g.points[BLACK] == 1);
	CHECK(play(&g, "b1c3") == MOVE_CONTINUE);
	CHECK(play(&g, "d5d2") == MOVE_CONTINUE); // Queen takes pawn, check
	CHECK(play(&g, "c1d2") == MOVE_CONTINUE); // Bishop takes queen
	CHECK(g.points[WHITE] == 10 && g.points[BLACK] == 2);

	// Invalid moves score nothing
	CHECK(play(&g, "a7a4") == MOVE_INVALID_PATTERN && g.points[BLACK] == 2);

	// En passant scores the pawn
	CHECK(game_load_fen(&g, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1") == 0);
	CHECK(play(&g, "e5d6") == MOVE_CONTINUE && g.points[WHITE] == 1);

	// A capturing promotion scores the captured piece, not the promotion
	CHECK(game_load_fen(&g, "4r1k1/3P4/8/8/8/8/8/4K3 w - - 0 1") == 0);
	CHECK(play_as(&g, "d7e8", QUEEN) == MOVE_CONTINUE && g.points[WHITE] == 5);
}

static void test_special_moves(void)
{
	struct Game g;

	// En passant
	standard(&g);
	CHECK(play(&g, "e2e4") == MOVE_CONTINUE);
	CHECK(play(&g, "a7a6") == MOVE_CONTINUE);
	CHECK(play(&g, "e4e5") == MOVE_CONTINUE);
	CHECK(play(&g, "d7d5") == MOVE_CONTINUE);
	CHECK(play(&g, "e5d6") == MOVE_CONTINUE);
	CHECK(at(&g, "d5") == SQ_EMPTY);
	CHECK(at(&g, "d6") == SQ_MAKE(PAWN, WHITE));

	// En passant expires after one move
	standard(&g);
	play(&g, "e2e4"); play(&g, "a7a6"); play(&g, "e4e5"); play(&g, "d7d5");
	play(&g, "h2h3"); play(&g, "h7h6");
	CHECK(play(&g, "e5d6") == MOVE_INVALID_PATTERN);

	// Castling both ways
	CHECK(game_load_fen(&g, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1") == 0);
	CHECK(play(&g, "e1g1") == MOVE_CONTINUE);
	CHECK(at(&g, "g1") == SQ_MAKE(KING, WHITE));
	CHECK(at(&g, "f1") == SQ_MAKE(ROOK, WHITE));
	CHECK(at(&g, "h1") == SQ_EMPTY);
	CHECK(play(&g, "e8c8") == MOVE_CONTINUE);
	CHECK(at(&g, "c8") == SQ_MAKE(KING, BLACK));
	CHECK(at(&g, "d8") == SQ_MAKE(ROOK, BLACK));

	// Castling through an attacked square
	CHECK(game_load_fen(&g, "r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1") == 0);
	CHECK(play(&g, "e1g1") == MOVE_INVALID_PATTERN);
	CHECK(play(&g, "e1c1") == MOVE_CONTINUE);

	// Castling rights are lost after the rook moves
	CHECK(game_load_fen(&g, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1") == 0);
	play(&g, "h1h2"); play(&g, "a8a7"); play(&g, "h2h1"); play(&g, "a7a8");
	CHECK(play(&g, "e1g1") == MOVE_INVALID_PATTERN);

	// Promotion to each allowed piece
	static const u8 promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
	for (unsigned i = 0; i < sizeof promos; i++) {
		// Black's h6 pawn keeps K + minor v K from being a dead position
		CHECK(game_load_fen(&g, "8/P6k/7p/8/8/8/8/K7 w - - 0 1") == 0);
		CHECK(play_as(&g, "a7a8", promos[i]) == MOVE_CONTINUE);
		CHECK(at(&g, "a8") == SQ_MAKE(promos[i], WHITE));
	}

	// Promotion by capture, using the text form
	CHECK(game_load_fen(&g, "1r5k/P7/7p/8/8/8/8/K7 w - - 0 1") == 0);
	CHECK(play(&g, "a7b8n") == MOVE_CONTINUE);
	CHECK(at(&g, "b8") == SQ_MAKE(KNIGHT, WHITE));
}

static void test_piece_type(void)
{
	struct Game g, before;

	// Non-promoting moves must name the moving piece's own type
	standard(&g);
	before = g;
	CHECK(play_as(&g, "g1f3", BISHOP) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "e2e4", QUEEN) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "e2e4", PIECE_NONE) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "e2e4", PIECE_TYPE_COUNT) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "e2e4", 0xFF) == MOVE_INVALID_PIECE_TYPE);
	CHECK(memcmp(&g, &before, sizeof g) == 0);
	CHECK(play_as(&g, "g1f3", KNIGHT) == MOVE_CONTINUE);

	// An unreachable square reports PATTERN, even with a wrong type
	standard(&g);
	CHECK(play_as(&g, "g1g3", BISHOP) == MOVE_INVALID_PATTERN);

	// Promotions must name an allowed piece, never a pawn or king
	CHECK(game_load_fen(&g, "8/P6k/8/8/8/8/8/K7 w - - 0 1") == 0);
	CHECK(play_as(&g, "a7a8", PAWN) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "a7a8", KING) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play_as(&g, "a7a8", PIECE_NONE) == MOVE_INVALID_PIECE_TYPE);
	CHECK(play(&g, "a7a8") == MOVE_INVALID_PIECE_TYPE); // Board fills in PAWN
}

static void test_parse_format(void)
{
	struct Game g;
	struct Move m;
	char buf[MOVE_TEXT_MAX];

	CHECK(move_parse("e2e4", &m) == 0);
	CHECK(m.start_x == 4 && m.start_y == 1 && m.end_x == 4 && m.end_y == 3);
	CHECK(m.piece == PIECE_NONE);
	CHECK(move_parse("e7e8q", &m) == 0 && m.piece == QUEEN);
	CHECK(move_parse("e7e8N", &m) == 0 && m.piece == KNIGHT);
	CHECK(move_parse("e7e8x", &m) == -1);
	CHECK(move_parse(" E2-E4\n", &m) == 0);
	CHECK(move_parse("e2 e4", &m) == 0);
	CHECK(move_parse("e2e9", &m) == 0); // Parses; the board size is checked by game_move
	standard(&g);
	CHECK(play(&g, "e2e9") == MOVE_INVALID_BOUNDS);
	CHECK(move_parse("e2e4x", &m) == -1);
	CHECK(move_parse("", &m) == -1);

	standard(&g);
	m = (struct Move){6, 0, 5, 2, PIECE_NONE};
	game_fill_piece(&g, &m);
	CHECK(m.piece == KNIGHT);
	move_format(&g, &m, buf);
	CHECK(strcmp(buf, "g1f3") == 0);

	CHECK(game_load_fen(&g, "8/P6k/8/8/8/8/8/K7 w - - 0 1") == 0);
	m = (struct Move){0, 6, 0, 7, ROOK};
	move_format(&g, &m, buf);
	CHECK(strcmp(buf, "a7a8r") == 0);
}

static void test_error_codes(void)
{
	CHECK(move_error_code(MOVE_CONTINUE) == 0);
	CHECK(move_error_code(MOVE_VICTORY) == 0);
	CHECK(move_error_code(MOVE_DRAW) == 0);
	CHECK(move_error_code(MOVE_INVALID_BOUNDS) == ERR_MOVE_BOUNDS);
	CHECK(move_error_code(MOVE_INVALID_NOT_OWN) == ERR_MOVE_NOT_OWN);
	CHECK(move_error_code(MOVE_INVALID_PATTERN) == ERR_MOVE_PATTERN);
	CHECK(move_error_code(MOVE_INVALID_SELF_CHECK) == ERR_MOVE_SELF_CHECK);
	CHECK(move_error_code(MOVE_INVALID_GAME_OVER) == ERR_MOVE_GAME_OVER);
	CHECK(move_error_code(MOVE_INVALID_PIECE_TYPE) == ERR_MOVE_PIECE_TYPE);

	// Every invalid result maps to a distinct code in the Move family.
	for (int r = MOVE_INVALID_GAME_OVER; r <= MOVE_INVALID_PIECE_TYPE; r++) {
		CHECK(ERR_FAMILY(move_error_code(r)) == ERR_FAMILY_MOVE);
		for (int s = r + 1; s <= MOVE_INVALID_PIECE_TYPE; s++)
			CHECK(move_error_code(r) != move_error_code(s));
	}
	CHECK(ERR_FAMILY(ERR_UNKNOWN_TYPE) == ERR_FAMILY_PROTOCOL);
}

static void test_messages(void)
{
	// Existing type numbers are unchanged; DRAW is appended
	CHECK(MSG_DISCONNECT == 9 && MSG_DRAW == 10 && MSG_COUNT == 11);
	CHECK(PAYLOAD_SIZE[MSG_DRAW] == 2);
	CHECK(PAYLOAD_SIZE[MSG_GO] == 2);

	// GO time and DRAW turn are two bytes, most significant first
	struct Go go;
	be16_set(go.time, CLOCK_DEFAULT_SECONDS);
	CHECK(go.time[0] == 0x02 && go.time[1] == 0x58);
	CHECK(be16_get(go.time) == 600);
	be16_set(go.time, CLOCK_MAX_SECONDS);
	CHECK(go.time[0] == 0xFF && go.time[1] == 0xFF && be16_get(go.time) == 65535);
	struct Draw d;
	be16_set(d.turn, 65536 + 3); // Turn numbers wrap
	CHECK(d.turn[0] == 0 && d.turn[1] == 3 && be16_get(d.turn) == 3);

	// Player IDs: 1-15 printable ASCII characters, then NUL
	u8 id[ID_MAX_SIZE] = "Xavier";
	CHECK(player_id_valid(id));
	memcpy(id, "A B~", 5);                  // Space and tilde are printable
	CHECK(player_id_valid(id));
	memset(id, 0, sizeof id);               // Empty
	CHECK(!player_id_valid(id));
	memcpy(id, "tab\there", 9);            // Control character
	CHECK(!player_id_valid(id));
	memcpy(id, "caf\xc3\xa9", 6);         // Non-ASCII (UTF-8)
	CHECK(!player_id_valid(id));
	memcpy(id, "del\x7f", 5);              // DEL is not printable
	CHECK(!player_id_valid(id));
	memset(id, 'a', sizeof id);             // No NUL terminator
	CHECK(!player_id_valid(id));
	id[ID_MAX_LENGTH] = 0;                  // 15 characters is the maximum
	CHECK(player_id_valid(id));

	// END: victory, then the receiver's points (two bytes, most significant first)
	CHECK(offsetof(struct End, points) == 1 && PAYLOAD_SIZE[MSG_END] == 3);

	// The turn number counts accepted moves only
	struct Game g;
	standard(&g);
	CHECK(g.ply == 0);
	CHECK(play(&g, "e2e5") == MOVE_INVALID_PATTERN && g.ply == 0);
	CHECK(play(&g, "e2e4") == MOVE_CONTINUE && g.ply == 1);
	CHECK(play(&g, "e7e5") == MOVE_CONTINUE && g.ply == 2);
}

int main(void)
{
	test_messages();
	test_error_codes();
	test_start_layout();
	test_large_boards();
	test_invalid_moves();
	test_checkmate();
	test_draws();
	test_special_moves();
	test_points();
	test_piece_type();
	test_parse_format();
	test_perft();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("All engine tests passed\n");
	return 0;
}
