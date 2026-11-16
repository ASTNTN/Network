#include "engine.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Piece movement is described by data, so fairy pieces can be added by
 * extending enum PieceType and PIECES[] without touching the move generator.
 * Pawns and castling are the only hard-coded special cases.
 *
 * The position is a list of at most MAX_PIECES pieces rather than a grid,
 * so a 255x255 board costs no more to copy than an 8x8 one. Ray moves find
 * their nearest blocker in one pass over the list instead of walking squares.
 */

enum { // Piece flags
	PF_PAWN    = 1 << 0, // Uses pawn movement instead of rules[]
	PF_ROYAL   = 1 << 1, // Must not be left in check; checkmating it wins
	PF_CASTLES = 1 << 2, // May castle with a rook
	PF_PROMO   = 1 << 3  // A pawn may promote to this piece
};

struct MoveRule {
	u8 count;    // Number of directions in d[]
	u8 range;    // Maximum steps per direction; 0 = unlimited (slider)
	i8 d[8][2];  // Direction vectors (dx, dy)
};

struct PieceDef {
	char symbol; // Uppercase letter, used for display and FEN
	u8 flags;    // PF_*
	u8 nrules;
	struct MoveRule rules[2]; // Compound pieces combine rules (e.g. bishop + knight)
	u8 value;    // Material points for capturing it
};

#define DIRS_ORTHO  {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}
#define DIRS_DIAG   {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}}
#define DIRS_ALL    {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}
#define DIRS_KNIGHT {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}}

static const struct PieceDef PIECES[PIECE_TYPE_COUNT] = {
	[PAWN]   = {'P', PF_PAWN, 0, {{0}}, 1},
	[KNIGHT] = {'N', PF_PROMO, 1, {{8, 1, DIRS_KNIGHT}}, 3},
	[BISHOP] = {'B', PF_PROMO, 1, {{4, 0, DIRS_DIAG}}, 3},
	[ROOK]   = {'R', PF_PROMO, 1, {{4, 0, DIRS_ORTHO}}, 5},
	[QUEEN]  = {'Q', PF_PROMO, 1, {{8, 0, DIRS_ALL}}, 9},
	[KING]   = {'K', PF_ROYAL | PF_CASTLES, 1, {{8, 1, DIRS_ALL}}, 0},
};

unsigned piece_value(u8 type)
{
	return type > PIECE_NONE && type < PIECE_TYPE_COUNT ? PIECES[type].value : 0;
}

const char *const STANDARD_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

/* ---- Helpers ---- */

static int min_int(int a, int b)
{
	return a < b ? a : b;
}

static int on_board(const struct Game *g, int x, int y)
{
	return x >= 0 && x < g->width && y >= 0 && y < g->height;
}

static int pawn_dir(u8 color)
{
	return color == WHITE ? 1 : -1;
}

static int home_row(const struct Game *g, u8 color)
{
	return color == WHITE ? 0 : g->height - 1;
}

static int valid_type(u8 type)
{
	return type > PIECE_NONE && type < PIECE_TYPE_COUNT;
}

static u8 piece_from_symbol(char c)
{
	for (u8 t = 1; t < PIECE_TYPE_COUNT; t++)
		if (PIECES[t].symbol == c)
			return t;
	return PIECE_NONE;
}

static const struct PieceDef *def_of(const struct PieceState *p)
{
	return &PIECES[SQ_TYPE(p->code)];
}

// Index of the piece on (x, y), or -1.
static int find_piece(const struct Game *g, int x, int y)
{
	for (int i = 0; i < g->npieces; i++)
		if (g->pieces[i].x == x && g->pieces[i].y == y)
			return i;
	return -1;
}

u8 game_piece_at(const struct Game *g, int x, int y)
{
	int i = on_board(g, x, y) ? find_piece(g, x, y) : -1;
	return i < 0 ? SQ_EMPTY : g->pieces[i].code;
}

static void remove_piece(struct Game *g, int i)
{
	g->pieces[i] = g->pieces[--g->npieces];
}

/* ---- Ray geometry ---- */

// The k >= 1 with (x, y) + k * (dx, dy) == (tx, ty), or 0 if there is none.
static int ray_steps(int x, int y, int dx, int dy, int tx, int ty)
{
	int ddx = tx - x, ddy = ty - y, k;
	if (dx != 0) {
		if (ddx % dx != 0)
			return 0;
		k = ddx / dx;
	} else {
		if (ddx != 0 || dy == 0 || ddy % dy != 0)
			return 0;
		k = ddy / dy;
	}
	return k >= 1 && ddx == k * dx && ddy == k * dy ? k : 0;
}

// Steps from (x, y) along (dx, dy) before leaving the board, capped at `range` (0 = unlimited).
static int ray_limit(const struct Game *g, int x, int y, int dx, int dy, int range)
{
	int lim = range ? range : BOARD_MAX;
	if (dx > 0) lim = min_int(lim, (g->width - 1 - x) / dx);
	if (dx < 0) lim = min_int(lim, x / -dx);
	if (dy > 0) lim = min_int(lim, (g->height - 1 - y) / dy);
	if (dy < 0) lim = min_int(lim, y / -dy);
	return lim;
}

// Steps to the nearest piece along a ray, within `max` steps, or 0 if the
// ray is clear. Stores that piece's index in `*idx`.
static int ray_blocker(const struct Game *g, int x, int y, int dx, int dy, int max, int *idx)
{
	int best = 0;
	for (int i = 0; i < g->npieces; i++) {
		int k = ray_steps(x, y, dx, dy, g->pieces[i].x, g->pieces[i].y);
		if (k && k <= max && (!best || k < best)) {
			best = k;
			*idx = i;
		}
	}
	return best;
}

/* ---- Attack detection ---- */

// Whether piece `i` attacks (tx, ty).
static int attacks(const struct Game *g, int i, int tx, int ty)
{
	const struct PieceState *p = &g->pieces[i];
	const struct PieceDef *def = def_of(p);

	if (def->flags & PF_PAWN)
		return ty == p->y + pawn_dir(SQ_COLOR(p->code)) && abs(tx - p->x) == 1;

	for (int r = 0; r < def->nrules; r++) {
		const struct MoveRule *rule = &def->rules[r];
		for (int d = 0; d < rule->count; d++) {
			int dx = rule->d[d][0], dy = rule->d[d][1], blk;
			int k = ray_steps(p->x, p->y, dx, dy, tx, ty);
			if (!k || (rule->range && k > rule->range))
				continue;
			if (k == 1 || !ray_blocker(g, p->x, p->y, dx, dy, k - 1, &blk))
				return 1;
		}
	}
	return 0;
}

static int square_attacked(const struct Game *g, int tx, int ty, u8 by)
{
	for (int i = 0; i < g->npieces; i++)
		if (SQ_COLOR(g->pieces[i].code) == by && attacks(g, i, tx, ty))
			return 1;
	return 0;
}

int game_in_check(const struct Game *g, u8 color)
{
	for (int i = 0; i < g->npieces; i++) {
		const struct PieceState *p = &g->pieces[i];
		if (SQ_COLOR(p->code) == color && (def_of(p)->flags & PF_ROYAL)
		    && square_attacked(g, p->x, p->y, !color))
			return 1;
	}
	return 0;
}

/* ---- Move generation ---- */

// Called for each generated move; returns nonzero to stop generation.
typedef int (*MoveFn)(const struct Game *g, const struct Move *m, void *ctx);

struct Gen {
	const struct Game *g;
	MoveFn fn;
	void *ctx;
};

static int emit(const struct Gen *gen, int sx, int sy, int ex, int ey, u8 piece)
{
	struct Move m = {(u8)sx, (u8)sy, (u8)ex, (u8)ey, piece};
	return gen->fn(gen->g, &m, gen->ctx);
}

// Emit a pawn move, expanding it into one move per promotion choice on the last row.
static int emit_pawn(const struct Gen *gen, int sx, int sy, int ex, int ey)
{
	if (ey != home_row(gen->g, !gen->g->turn))
		return emit(gen, sx, sy, ex, ey, PAWN);
	for (u8 t = 1; t < PIECE_TYPE_COUNT; t++)
		if ((PIECES[t].flags & PF_PROMO) && emit(gen, sx, sy, ex, ey, t))
			return 1;
	return 0;
}

static int gen_pawn(const struct Gen *gen, int i)
{
	const struct Game *g = gen->g;
	const struct PieceState *p = &g->pieces[i];
	u8 color = SQ_COLOR(p->code);
	int dir = pawn_dir(color);
	int x = p->x, y = p->y, fy = y + dir;

	if (!on_board(g, x, fy))
		return 0;

	if (find_piece(g, x, fy) < 0) {
		if (emit_pawn(gen, x, y, x, fy))
			return 1;
		int fy2 = fy + dir;
		if (!p->moved && on_board(g, x, fy2) && find_piece(g, x, fy2) < 0
		    && emit_pawn(gen, x, y, x, fy2))
			return 1;
	}

	for (int dx = -1; dx <= 1; dx += 2) {
		int cx = x + dx;
		if (!on_board(g, cx, fy))
			continue;
		int t = find_piece(g, cx, fy);
		int capture = t >= 0 && SQ_COLOR(g->pieces[t].code) != color;
		int ep = g->ep_valid && g->ep_x == cx && g->ep_y == fy;
		if ((capture || ep) && emit_pawn(gen, x, y, cx, fy))
			return 1;
	}
	return 0;
}

/*
 * Castling, generalized to any board: an unmoved king that is not in check
 * moves two squares toward an unmoved rook on its row, at least three
 * squares away, with only empty squares between them. The king may not
 * pass through or land on an attacked square. The rook lands on the square
 * the king passed over.
 */
static int gen_castles(const struct Gen *gen, int i)
{
	const struct Game *g = gen->g;
	const struct PieceState *k = &g->pieces[i];
	u8 color = SQ_COLOR(k->code);

	if (k->moved || square_attacked(g, k->x, k->y, !color))
		return 0;

	for (int s = -1; s <= 1; s += 2) {
		int blk;
		int dist = ray_blocker(g, k->x, k->y, s, 0, BOARD_MAX, &blk);
		if (dist < 3)
			continue;
		const struct PieceState *rook = &g->pieces[blk];
		if (rook->code != SQ_MAKE(ROOK, color) || rook->moved)
			continue;
		if (square_attacked(g, k->x + s, k->y, !color) || square_attacked(g, k->x + 2 * s, k->y, !color))
			continue;
		if (emit(gen, k->x, k->y, k->x + 2 * s, k->y, KING))
			return 1;
	}
	return 0;
}

// All moves for piece `i`, ignoring whether they leave the king in check.
static int gen_piece(const struct Gen *gen, int i)
{
	const struct Game *g = gen->g;
	const struct PieceState *p = &g->pieces[i];
	const struct PieceDef *def = def_of(p);

	if (def->flags & PF_PAWN)
		return gen_pawn(gen, i);

	for (int r = 0; r < def->nrules; r++) {
		const struct MoveRule *rule = &def->rules[r];
		for (int d = 0; d < rule->count; d++) {
			int dx = rule->d[d][0], dy = rule->d[d][1], blk = -1;
			int lim = ray_limit(g, p->x, p->y, dx, dy, rule->range);
			int hit = lim ? ray_blocker(g, p->x, p->y, dx, dy, lim, &blk) : 0;
			int end = hit ? hit : lim;
			for (int step = 1; step <= end; step++) {
				if (step == hit && SQ_COLOR(g->pieces[blk].code) == SQ_COLOR(p->code))
					break; // Own piece
				if (emit(gen, p->x, p->y, p->x + step * dx, p->y + step * dy, SQ_TYPE(p->code)))
					return 1;
			}
		}
	}

	if (def->flags & PF_CASTLES)
		return gen_castles(gen, i);
	return 0;
}

// All moves for the side to move. Returns 1 if the callback stopped generation.
static int gen_all(const struct Gen *gen)
{
	for (int i = 0; i < gen->g->npieces; i++)
		if (SQ_COLOR(gen->g->pieces[i].code) == gen->g->turn && gen_piece(gen, i))
			return 1;
	return 0;
}

/* ---- Making moves ---- */

// Apply a pseudo-legal move without validating it.
static void make_move(struct Game *g, const struct Move *m)
{
	int sx = m->start_x, sy = m->start_y, ex = m->end_x, ey = m->end_y;
	int mi = find_piece(g, sx, sy);
	u8 color = SQ_COLOR(g->pieces[mi].code);
	u8 flags = def_of(&g->pieces[mi])->flags;

	int ci = find_piece(g, ex, ey);
	if ((flags & PF_PAWN) && ex != sx && ci < 0)
		ci = find_piece(g, ex, sy); // En passant capture

	if ((flags & PF_CASTLES) && ey == sy && abs(ex - sx) == 2) {
		int s = ex > sx ? 1 : -1, ri;
		ray_blocker(g, sx, sy, s, 0, BOARD_MAX, &ri); // The castling rook
		g->pieces[ri].x = (u8)(sx + s);
		g->pieces[ri].moved = 1;
	}

	if (ci >= 0) {
		g->points[color] += piece_value(SQ_TYPE(g->pieces[ci].code));
		remove_piece(g, ci);
		if (mi == g->npieces) // The mover was last, so it now sits at ci
			mi = ci;
	}

	struct PieceState *p = &g->pieces[mi];
	p->x = (u8)ex;
	p->y = (u8)ey;
	p->code = SQ_MAKE(m->piece, color); // Differs from the mover on promotion
	p->moved = 1;

	g->ep_valid = (flags & PF_PAWN) && abs(ey - sy) == 2;
	g->ep_x = (u8)sx;
	g->ep_y = (u8)((sy + ey) / 2);
	g->turn = !g->turn;
	g->ply++;
}

static int is_legal(const struct Game *g, const struct Move *m)
{
	struct Game next = *g;
	make_move(&next, m);
	return !game_in_check(&next, g->turn);
}

static int stop_if_legal(const struct Game *g, const struct Move *m, void *ctx)
{
	(void)ctx;
	return is_legal(g, m);
}

static int has_legal_move(const struct Game *g)
{
	struct Gen gen = {g, stop_if_legal, NULL};
	return gen_all(&gen);
}

struct ListCtx {
	struct Move *out;
	int max;
	int n;
};

static int collect_legal(const struct Game *g, const struct Move *m, void *ctx)
{
	struct ListCtx *list = ctx;
	if (is_legal(g, m)) {
		if (list->n < list->max)
			list->out[list->n] = *m;
		list->n++;
	}
	return 0;
}

int game_legal_moves(const struct Game *g, struct Move *out, int max)
{
	struct ListCtx list = {out, max, 0};
	struct Gen gen = {g, collect_legal, &list};
	gen_all(&gen);
	return list.n;
}

/* ---- Game outcome ---- */

// Insufficient material: neither side can ever deliver checkmate.
// Covers K v K, K + minor piece v K, and kings with bishops all on one square color.
static int dead_position(const struct Game *g)
{
	int knights = 0, bishops = 0;
	int bishop_colors = 0; // Bit per square color

	for (int i = 0; i < g->npieces; i++) {
		const struct PieceState *p = &g->pieces[i];
		u8 type = SQ_TYPE(p->code);
		if (type == KING)
			continue;
		if (type == KNIGHT) {
			knights++;
		} else if (type == BISHOP) {
			bishops++;
			bishop_colors |= 1 << ((p->x + p->y) & 1);
		} else {
			return 0;
		}
	}

	if (knights + bishops <= 1)
		return 1;
	return knights == 0 && bishop_colors != 3;
}

struct MatchCtx {
	const struct Move *want;
	int squares_match;
	int found;
	struct Move match;
};

static int match_move(const struct Game *g, const struct Move *m, void *ctx)
{
	struct MatchCtx *mc = ctx;
	(void)g;
	if (m->start_x != mc->want->start_x || m->start_y != mc->want->start_y
	    || m->end_x != mc->want->end_x || m->end_y != mc->want->end_y)
		return 0;
	mc->squares_match = 1;
	if (m->piece != mc->want->piece)
		return 0;
	mc->found = 1;
	mc->match = *m;
	return 1;
}

enum MoveResult game_move(struct Game *g, const struct Move *m)
{
	if (g->status != STATUS_ONGOING)
		return MOVE_INVALID_GAME_OVER;
	if (!on_board(g, m->start_x, m->start_y) || !on_board(g, m->end_x, m->end_y))
		return MOVE_INVALID_BOUNDS;

	int mi = find_piece(g, m->start_x, m->start_y);
	if (mi < 0 || SQ_COLOR(g->pieces[mi].code) != g->turn)
		return MOVE_INVALID_NOT_OWN;

	// Match the squares first, then the piece type, so a move to an
	// unreachable square reports PATTERN rather than PIECE_TYPE.
	struct MatchCtx mc = {m, 0, 0, {0}};
	struct Gen gen = {g, match_move, &mc};
	gen_piece(&gen, mi);
	if (!mc.squares_match)
		return MOVE_INVALID_PATTERN;
	if (!mc.found)
		return MOVE_INVALID_PIECE_TYPE;
	if (!is_legal(g, &mc.match))
		return MOVE_INVALID_SELF_CHECK;
	make_move(g, &mc.match);

	if (!has_legal_move(g)) {
		if (game_in_check(g, g->turn)) { // Checkmate
			g->status = STATUS_WON;
			g->winner = !g->turn;
			return MOVE_VICTORY;
		}
		g->status = STATUS_DRAWN; // Stalemate
		return MOVE_DRAW;
	}
	if (dead_position(g)) {
		g->status = STATUS_DRAWN;
		return MOVE_DRAW;
	}
	return MOVE_CONTINUE;
}

/* ---- Setup ---- */

u8 game_from_start(struct Game *g, const struct Start *s)
{
	memset(g, 0, sizeof *g);
	if (s->cols == 0 || s->rows == 0)
		return ERR_CONFIG_SIZE;
	if (s->count > MAX_PIECES)
		return ERR_CONFIG_COUNT;
	if (s->color != WHITE && s->color != BLACK)
		return ERR_CONFIG_COLOR;
	g->width = s->cols;
	g->height = s->rows;

	int kings[2] = {0, 0};
	for (int i = 0; i < s->count; i++) {
		const struct Placement *pl = &s->pieces[i];
		u8 type = SQ_TYPE(pl->piece), color = SQ_COLOR(pl->piece);
		if (!valid_type(type))
			return ERR_CONFIG_PIECE_TYPE;
		if (pl->col >= s->cols || pl->row >= s->rows || find_piece(g, pl->col, pl->row) >= 0)
			return ERR_CONFIG_PLACEMENT;
		if ((PIECES[type].flags & PF_PAWN) && pl->row == home_row(g, !color))
			return ERR_CONFIG_PAWN_PROMOTION;
		g->pieces[g->npieces++] = (struct PieceState){pl->piece, pl->col, pl->row, 0};
		if (PIECES[type].flags & PF_ROYAL)
			kings[color]++;
	}
	if (!kings[WHITE] || !kings[BLACK])
		return ERR_CONFIG_NO_KING;
	if (kings[WHITE] > 1 || kings[BLACK] > 1)
		return ERR_CONFIG_MULTIPLE_KINGS;

	g->turn = WHITE;
	g->status = STATUS_ONGOING;
	if (!has_legal_move(g) || dead_position(g))
		return ERR_CONFIG_FINISHED;
	if (game_in_check(g, WHITE) || game_in_check(g, BLACK))
		return ERR_CONFIG_IN_CHECK;
	return 0;
}

unsigned game_to_start(const struct Game *g, struct Start *s)
{
	s->cols = g->width;
	s->rows = g->height;
	s->count = g->npieces;
	for (int i = 0; i < g->npieces; i++) {
		const struct PieceState *p = &g->pieces[i];
		s->pieces[i] = (struct Placement){p->code, p->y, p->x};
	}
	return start_payload_size(s->count);
}

// Parse a square like "e2" or "aa10". Columns are bijective base 26 (a..z, aa, ab, ...).
static int parse_square(const char **s, u8 *x, u8 *y)
{
	const char *p = *s;
	int col = 0, row = 0;

	if (!isalpha((unsigned char)*p))
		return -1;
	while (isalpha((unsigned char)*p)) {
		col = col * 26 + (tolower((unsigned char)*p++) - 'a' + 1);
		if (col > BOARD_MAX)
			return -1;
	}
	if (!isdigit((unsigned char)*p) || *p == '0')
		return -1;
	while (isdigit((unsigned char)*p)) {
		row = row * 10 + (*p++ - '0');
		if (row > BOARD_MAX)
			return -1;
	}

	*x = (u8)(col - 1);
	*y = (u8)(row - 1);
	*s = p;
	return 0;
}

int game_load_fen(struct Game *g, const char *fen)
{
	struct { u8 code; int x, r; } placed[MAX_PIECES]; // r counts rows from the top
	int n = 0, r = 0, x = 0, width = -1;

	memset(g, 0, sizeof *g);

	// Piece placement, from the top row down
	for (; *fen && *fen != ' '; fen++) {
		char c = *fen;
		if (c == '/') {
			if (width < 0)
				width = x;
			else if (x != width)
				return -1;
			if (++r >= BOARD_MAX)
				return -1;
			x = 0;
		} else if (isdigit((unsigned char)c)) {
			int empty = 0;
			while (isdigit((unsigned char)fen[0])) {
				empty = empty * 10 + (*fen++ - '0');
				if (empty > BOARD_MAX)
					return -1;
			}
			fen--;
			if (empty == 0)
				return -1;
			x += empty;
		} else {
			u8 type = piece_from_symbol((char)toupper((unsigned char)c));
			if (type == PIECE_NONE || n >= MAX_PIECES)
				return -1;
			placed[n].code = SQ_MAKE(type, isupper((unsigned char)c) ? WHITE : BLACK);
			placed[n].x = x++;
			placed[n++].r = r;
		}
		if (x > BOARD_MAX)
			return -1;
	}
	if (width < 0)
		width = x;
	if (x != width || width == 0 || *fen++ != ' ')
		return -1;

	g->width = (u8)width;
	g->height = (u8)(r + 1);
	for (int i = 0; i < n; i++) {
		struct PieceState *p = &g->pieces[g->npieces++];
		p->code = placed[i].code;
		p->x = (u8)placed[i].x;
		p->y = (u8)(g->height - 1 - placed[i].r);

		// A pawn off its second row must have moved. Kings and rooks start
		// as moved; the castling field below re-enables the ones that may castle.
		u8 color = SQ_COLOR(p->code), type = SQ_TYPE(p->code);
		if (type == PAWN)
			p->moved = p->y != home_row(g, color) + pawn_dir(color);
		else if (type == KING || type == ROOK)
			p->moved = 1;
	}

	// Side to move
	if (*fen == 'w')
		g->turn = WHITE;
	else if (*fen == 'b')
		g->turn = BLACK;
	else
		return -1;
	if (*++fen != ' ')
		return -1;
	fen++;

	// Castling rights: K/Q (k/q) re-enable White's (Black's) king and the rook
	// in the right/left corner of its home row.
	if (*fen == '-') {
		fen++;
	} else {
		for (; *fen && *fen != ' '; fen++) {
			u8 color = islower((unsigned char)*fen) ? BLACK : WHITE;
			char side = (char)toupper((unsigned char)*fen);
			if (side != 'K' && side != 'Q')
				return -1;
			int hy = home_row(g, color);
			int ri = find_piece(g, side == 'K' ? g->width - 1 : 0, hy);
			if (ri < 0 || g->pieces[ri].code != SQ_MAKE(ROOK, color))
				return -1;
			g->pieces[ri].moved = 0;
			for (int i = 0; i < g->npieces; i++)
				if (g->pieces[i].code == SQ_MAKE(KING, color) && g->pieces[i].y == hy)
					g->pieces[i].moved = 0;
		}
	}
	if (*fen++ != ' ')
		return -1;

	// En passant target
	if (*fen != '-') {
		if (parse_square(&fen, &g->ep_x, &g->ep_y) != 0 || !on_board(g, g->ep_x, g->ep_y))
			return -1;
		g->ep_valid = 1;
	}
	// Halfmove and fullmove counters are not tracked.

	g->status = STATUS_ONGOING;
	return 0;
}

/* ---- Text I/O ---- */

// Column label in bijective base 26: a..z, aa, ab, ... Returns its length.
static int column_label(int x, char *buf)
{
	char tmp[4];
	int len = 0;
	for (int n = x + 1; n > 0; n = (n - 1) / 26)
		tmp[len++] = (char)('a' + (n - 1) % 26);
	for (int i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	buf[len] = '\0';
	return len;
}

static char piece_char(u8 code)
{
	if (code == SQ_EMPTY)
		return '.';
	char c = PIECES[SQ_TYPE(code)].symbol;
	return SQ_COLOR(code) == BLACK ? (char)tolower((unsigned char)c) : c;
}

void game_print(const struct Game *g, FILE *f, u8 perspective)
{
	int flip = perspective == BLACK;
	int cw = g->width > 26 ? 2 : 1;                          // Cell width
	int rw = g->height >= 100 ? 3 : g->height >= 10 ? 2 : 1; // Row label width
	char label[4];

	fprintf(f, "%*s +", rw, "");
	for (int col = 0; col <= g->width; col++)
		fprintf(f, "%.*s", col < g->width ? cw + 1 : 1, "---");
	fprintf(f, "+\n");

	for (int row = 0; row < g->height; row++) {
		int y = flip ? row : g->height - 1 - row;
		fprintf(f, "%*d |", rw, y + 1);
		for (int col = 0; col < g->width; col++) {
			int x = flip ? g->width - 1 - col : col;
			fprintf(f, " %*c", cw, piece_char(game_piece_at(g, x, y)));
		}
		fprintf(f, " |\n");
	}

	fprintf(f, "%*s +", rw, "");
	for (int col = 0; col <= g->width; col++)
		fprintf(f, "%.*s", col < g->width ? cw + 1 : 1, "---");
	fprintf(f, "+\n%*s  ", rw, "");
	for (int col = 0; col < g->width; col++) {
		column_label(flip ? g->width - 1 - col : col, label);
		fprintf(f, " %*s", cw, label);
	}
	fprintf(f, "\n");
}

int move_parse(const char *s, struct Move *out)
{
	while (isspace((unsigned char)*s))
		s++;
	if (parse_square(&s, &out->start_x, &out->start_y) != 0)
		return -1;
	if (*s == ' ' || *s == '-')
		s++;
	if (parse_square(&s, &out->end_x, &out->end_y) != 0)
		return -1;

	out->piece = PIECE_NONE;
	if (isalpha((unsigned char)*s)) {
		out->piece = piece_from_symbol((char)toupper((unsigned char)*s++));
		if (out->piece == PIECE_NONE)
			return -1;
	}

	while (isspace((unsigned char)*s))
		s++;
	return *s == '\0' ? 0 : -1;
}

void game_fill_piece(const struct Game *g, struct Move *m)
{
	if (m->piece == PIECE_NONE)
		m->piece = SQ_TYPE(game_piece_at(g, m->start_x, m->start_y));
}

void move_format(const struct Game *g, const struct Move *m, char *buf)
{
	char from[4], to[4];
	column_label(m->start_x, from);
	column_label(m->end_x, to);
	int len = snprintf(buf, MOVE_TEXT_MAX, "%s%d%s%d", from, m->start_y + 1, to, m->end_y + 1);

	u8 mover = SQ_TYPE(game_piece_at(g, m->start_x, m->start_y));
	if (m->piece != mover && valid_type(m->piece) && len + 1 < MOVE_TEXT_MAX) {
		buf[len] = (char)tolower((unsigned char)PIECES[m->piece].symbol);
		buf[len + 1] = '\0';
	}
}

u8 move_error_code(enum MoveResult r)
{
	switch (r) {
	case MOVE_CONTINUE:
	case MOVE_VICTORY:
	case MOVE_DRAW:               return 0;
	case MOVE_INVALID_GAME_OVER:  return ERR_MOVE_GAME_OVER;
	case MOVE_INVALID_BOUNDS:     return ERR_MOVE_BOUNDS;
	case MOVE_INVALID_NOT_OWN:    return ERR_MOVE_NOT_OWN;
	case MOVE_INVALID_PATTERN:    return ERR_MOVE_PATTERN;
	case MOVE_INVALID_SELF_CHECK: return ERR_MOVE_SELF_CHECK;
	case MOVE_INVALID_PIECE_TYPE: return ERR_MOVE_PIECE_TYPE;
	}
	return 0;
}

const char *move_result_str(enum MoveResult r)
{
	switch (r) {
	case MOVE_CONTINUE:           return "move accepted";
	case MOVE_VICTORY:            return "checkmate";
	case MOVE_DRAW:               return "draw";
	case MOVE_INVALID_GAME_OVER:  return "the game is over";
	case MOVE_INVALID_BOUNDS:     return "square is off the board";
	case MOVE_INVALID_NOT_OWN:    return "no piece of yours on that square";
	case MOVE_INVALID_PATTERN:    return "that piece cannot move there";
	case MOVE_INVALID_SELF_CHECK: return "move would leave your king in check";
	case MOVE_INVALID_PIECE_TYPE: return "wrong piece type for that move";
	}
	return "unknown result";
}
