/*
 * Fairy Chess server.
 *
 * Implements the server FSM (docs/fsm_specification.md) with the threading
 * design in docs/concurrency_design.md: the main thread runs the match
 * lifecycle (WAIT_P1, WAIT_P2, INIT, FINISH, CLEANUP) and refuses extra
 * connections; one worker per client does all of that client's socket I/O
 * and plays TURN/CLASSIFY while it owns the game state.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"
#include "engine.h"
#include "net.h"
#include "timing.h"

/* ---- Shared state (concurrency_design.md, Shared State) ---- */

enum { MAIN, CLIENT_1, CLIENT_2 }; // Ownership values

enum { // Client liveness
	SLOT_EMPTY,
	SLOT_CONNECTED,
	SLOT_JOINED,
	SLOT_DEAD
};

enum { // Pending outgoing messages, sent in this order
	PEND_START      = 1 << 0,
	PEND_UPDATE     = 1 << 1,
	PEND_GO         = 1 << 2,
	PEND_DRAW       = 1 << 3,
	PEND_END        = 1 << 4,
	PEND_DISCONNECT = 1 << 5
};

struct Outbox {
	struct Start start;
	struct Update update;
	struct End end;
};

struct Slot {
	int sock;            // Written by main before the worker starts, then used only by the worker
	u8 id[ID_MAX_SIZE];  // Written by the worker before it stores SLOT_JOINED
	_Atomic u8 liveness;
	_Atomic u8 pending;
	struct Outbox outbox;
	pthread_t thread;    // Main thread only
	int running;         // Main thread only: whether `thread` must be joined
};

#define TIE (-1)

static _Atomic u8 owner;
static struct Slot slots[2];
static _Atomic uint32_t draw_state; // Turn << 2 | offer bit per slot
static _Atomic u8 config_tie;
static atomic_flag join_lock = ATOMIC_FLAG_INIT;

// Owned game state: accessed only by the thread whose value is in `owner`.
static struct Game game;
static u8 color_of[2];          // Each slot's colour
static unsigned clock_left[2];  // Remaining seconds per slot
static long long turn_start;    // When the ACTIVE slot's GO was sent
static int winner;              // Slot index, or TIE

static volatile sig_atomic_t exit_requested;
static int exit_noted; // Main thread only

static void on_sigint(int sig)
{
	(void)sig;
	exit_requested = 1;
}

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	va_list ap;
	va_start(ap, fmt);
	pthread_mutex_lock(&lock);
	fputs("[server] ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	fflush(stdout);
	pthread_mutex_unlock(&lock);
	va_end(ap);
}

static const char *slot_name(int i)
{
	return slots[i].id[0] ? (const char *)slots[i].id : (i == 0 ? "slot 1" : "slot 2");
}

static void queue(int i, u8 flags)
{
	atomic_fetch_or_explicit(&slots[i].pending, flags, memory_order_release);
}

static int owns(u8 me)
{
	return atomic_load_explicit(&owner, memory_order_acquire) == me;
}

static uint32_t draw_turn(void)
{
	return (uint32_t)(game.ply & 0xFFFF);
}

/* ---- Worker ---- */

struct Worker {
	int i, opp;
	u8 me;               // CLIENT_1 or CLIENT_2
	int sock;
	int alive;           // Socket I/O still allowed
	int in_game;         // Between sending START and END: DRAW and Configuration ERRORs count
	int clock_running;   // This client's turn has started (GO sent)
	long long last_rx, last_keepalive;
	struct Reader rd;
};

// Record the result and hand ownership to MAIN. Caller must own the game state.
static void end_game(struct Worker *w, int result)
{
	winner = result;
	w->clock_running = 0;
	atomic_store_explicit(&owner, MAIN, memory_order_release);
}

static void client_dead(struct Worker *w, const char *why)
{
	if (!w->alive)
		return;
	log_msg("%s: %s", slot_name(w->i), why);
	close(w->sock);
	w->alive = 0;
	atomic_store_explicit(&slots[w->i].liveness, SLOT_DEAD, memory_order_release);
}

static void send_or_die(struct Worker *w, u8 type, const void *payload, size_t len)
{
	if (w->alive && send_msg(w->sock, type, payload, len) < 0)
		client_dead(w, "send failed");
}

static void send_error(struct Worker *w, u8 code)
{
	struct Error e = {code};
	send_or_die(w, MSG_ERROR, &e, sizeof e);
}

// Fatal error: ERROR, then DISCONNECT, then close (protocol_blueprint.md, Fatal Errors).
static void fatal_error(struct Worker *w, u8 code, const char *why)
{
	send_error(w, code);
	send_or_die(w, MSG_DISCONNECT, NULL, 0);
	client_dead(w, why);
}

static void send_go(struct Worker *w, unsigned seconds)
{
	struct Go go;
	be16_set(go.time, seconds);
	send_or_die(w, MSG_GO, &go, sizeof go);
}

// New-turn GO from the outbox. The worker owns the game state here.
static void begin_turn(struct Worker *w)
{
	if (!owns(w->me))
		return;
	if (clock_left[w->i] == 0) { // Charged to zero by the one-second minimum
		log_msg("%s has no time left", slot_name(w->i));
		end_game(w, w->opp);
		return;
	}
	send_go(w, clock_left[w->i]);
	turn_start = now_ms();
	w->clock_running = 1;
}

static void flush_outbox(struct Worker *w)
{
	u8 p = atomic_exchange_explicit(&slots[w->i].pending, 0, memory_order_acquire);
	struct Outbox *o = &slots[w->i].outbox;

	if ((p & PEND_START) && w->alive) {
		send_or_die(w, MSG_START, &o->start, start_payload_size(o->start.count));
		w->in_game = 1;
	}
	if (p & PEND_UPDATE)
		send_or_die(w, MSG_UPDATE, &o->update, sizeof o->update);
	if ((p & PEND_GO) && w->alive)
		begin_turn(w);
	if (p & PEND_DRAW) {
		// Forward the opponent's offer only if it still stands
		uint32_t st = atomic_load_explicit(&draw_state, memory_order_acquire);
		if (st & (1u << w->opp)) {
			struct Draw d;
			be16_set(d.turn, st >> 2);
			send_or_die(w, MSG_DRAW, &d, sizeof d);
		}
	}
	if (p & PEND_END) {
		send_or_die(w, MSG_END, &o->end, sizeof o->end);
		w->in_game = 0;
	}
	if (p & PEND_DISCONNECT) {
		if (w->alive) {
			send_msg(w->sock, MSG_DISCONNECT, NULL, 0);
			close(w->sock);
			w->alive = 0;
		}
		pthread_exit(NULL);
	}
}

// Worker loop step 2 (and the dead-worker rule in Disconnects & Forfeits).
static void check_game_end(struct Worker *w)
{
	if (!owns(w->me))
		return;
	if (atomic_load_explicit(&config_tie, memory_order_acquire)) {
		end_game(w, TIE);
	} else if (atomic_load_explicit(&slots[w->opp].liveness, memory_order_acquire) == SLOT_DEAD) {
		// The flag is set before the slot is marked dead, so re-reading it is enough
		end_game(w, atomic_load_explicit(&config_tie, memory_order_acquire) ? TIE : w->i);
	} else if ((atomic_load_explicit(&draw_state, memory_order_acquire) & 3u) == 3u) {
		log_msg("Draw agreed");
		end_game(w, TIE);
	} else if (!w->alive) {
		end_game(w, atomic_load_explicit(&config_tie, memory_order_acquire) ? TIE : w->opp);
	} else if (w->clock_running && now_ms() - turn_start >= (long long)clock_left[w->i] * 1000) {
		log_msg("%s ran out of time", slot_name(w->i));
		end_game(w, w->opp);
	}
}

static void handle_connect(struct Worker *w, const struct Msg *m)
{
	struct Connect c;
	memcpy(&c, m->payload, sizeof c);

	if (!player_id_valid(c.id)) {
		send_error(w, ERR_BAD_PLAYER_ID);
		return;
	}
	u8 id[ID_MAX_SIZE] = {0};
	strcpy((char *)id, (const char *)c.id); // Valid IDs are NUL-terminated

	while (atomic_flag_test_and_set_explicit(&join_lock, memory_order_acquire))
		; // Held only for a comparison and two stores
	int dup = atomic_load_explicit(&slots[w->opp].liveness, memory_order_acquire) == SLOT_JOINED
	          && strcmp((const char *)slots[w->opp].id, (const char *)id) == 0;
	if (!dup) {
		memcpy(slots[w->i].id, id, sizeof id);
		atomic_store_explicit(&slots[w->i].liveness, SLOT_JOINED, memory_order_release);
	}
	atomic_flag_clear_explicit(&join_lock, memory_order_release);

	if (dup) {
		send_error(w, ERR_DUPLICATE_PLAYER_ID);
		return;
	}
	send_or_die(w, MSG_ACCEPT, NULL, 0);
	log_msg("%s joined", (const char *)id);
}

// CLASSIFY (fsm_specification.md), run by the ACTIVE client's worker.
static void handle_move(struct Worker *w, const struct Msg *m)
{
	if (!w->in_game) {
		send_error(w, ERR_UNEXPECTED_MESSAGE);
		return;
	}
	if (!owns(w->me) || !w->clock_running) {
		send_error(w, ERR_NOT_YOUR_TURN);
		return;
	}

	if (now_ms() - turn_start >= (long long)clock_left[w->i] * 1000) {
		log_msg("%s ran out of time", slot_name(w->i)); // Before the move arrived
		end_game(w, w->opp);
		return;
	}

	struct Move mv;
	memcpy(&mv, m->payload, sizeof mv);
	char text[MOVE_TEXT_MAX];
	move_format(&game, &mv, text);
	enum MoveResult r = game_move(&game, &mv);
	long long elapsed = (now_ms() - turn_start) / 1000; // Whole seconds

	if (MOVE_IS_INVALID(r)) {
		log_msg("%s: invalid move %s (%s)", slot_name(w->i), text, move_result_str(r));
		send_error(w, move_error_code(r));
		// The clock keeps running; the retry GO carries what is left (at least 1 s)
		send_go(w, clock_left[w->i] - (unsigned)elapsed);
		return;
	}

	log_msg("%s plays %s", slot_name(w->i), text);
	struct Update up;
	memcpy(&up, &mv, sizeof up);
	send_or_die(w, MSG_UPDATE, &up, sizeof up);
	slots[w->opp].outbox.update = up;

	if (r == MOVE_VICTORY || r == MOVE_DRAW) {
		queue(w->opp, PEND_UPDATE);
		log_msg(r == MOVE_VICTORY ? "Checkmate" : "Drawn position");
		end_game(w, r == MOVE_VICTORY ? w->i : TIE);
		return;
	}

	unsigned charge = elapsed < 1 ? 1 : (unsigned)elapsed;
	clock_left[w->i] -= charge < clock_left[w->i] ? charge : clock_left[w->i];
	atomic_store_explicit(&draw_state, draw_turn() << 2, memory_order_release); // Offers lapse
	w->clock_running = 0;
	atomic_store_explicit(&owner, (u8)(CLIENT_1 + w->opp), memory_order_release);
	queue(w->opp, PEND_UPDATE | PEND_GO);
}

static void handle_draw(struct Worker *w, const struct Msg *m)
{
	if (!w->in_game)
		return;
	struct Draw d;
	memcpy(&d, m->payload, sizeof d);
	uint32_t turn = be16_get(d.turn), mine = 1u << w->i, theirs = 1u << w->opp;

	uint32_t cur = atomic_load_explicit(&draw_state, memory_order_acquire);
	for (;;) {
		if ((cur >> 2) != turn || (cur & mine))
			return; // Another turn's offer, or a repeat
		if (atomic_compare_exchange_weak_explicit(&draw_state, &cur, cur | mine,
		                                          memory_order_acq_rel, memory_order_acquire))
			break;
	}
	if (!(cur & theirs)) {
		log_msg("%s offers a draw", slot_name(w->i));
		queue(w->opp, PEND_DRAW);
	} else if (owns(w->me)) {
		log_msg("Draw agreed");
		end_game(w, TIE);
	} // Otherwise the owning worker sees both bits at its next wake-up
}

static void handle_error(struct Worker *w, const struct Msg *m)
{
	u8 code = m->payload[0];
	log_msg("%s reports error 0x%02x (%s)", slot_name(w->i), code, error_str(code));
	if (!w->in_game || ERR_FAMILY(code) != ERR_FAMILY_CONFIG)
		return;
	atomic_store_explicit(&config_tie, 1, memory_order_release);
	if (owns(w->me))
		end_game(w, TIE);
}

static void handle_message(struct Worker *w, const struct Msg *m)
{
	int joined = atomic_load_explicit(&slots[w->i].liveness, memory_order_relaxed) == SLOT_JOINED;

	switch (m->type) {
	case MSG_KEEPALIVE:
		break; // Any message resets the keepalive timer
	case MSG_CONNECT:
		if (joined)
			send_error(w, ERR_UNEXPECTED_MESSAGE);
		else
			handle_connect(w, m);
		break;
	case MSG_MOVE:
		handle_move(w, m);
		break;
	case MSG_DRAW:
		handle_draw(w, m);
		break;
	case MSG_ERROR:
		handle_error(w, m);
		break;
	case MSG_DISCONNECT:
		client_dead(w, "disconnected");
		break;
	default:
		send_error(w, ERR_UNEXPECTED_MESSAGE);
	}
}

static void read_messages(struct Worker *w)
{
	if (reader_fill(&w->rd, w->sock) < 0) {
		client_dead(w, "connection closed");
		return;
	}
	struct Msg m;
	for (;;) {
		enum ReadStatus st = reader_next(&w->rd, &m);
		if (st == READ_NONE)
			return;
		if (st == READ_BAD_TYPE) {
			fatal_error(w, ERR_UNKNOWN_TYPE, "sent an unknown message type");
			return;
		}
		if (st == READ_BAD_LENGTH) { // Unframeable START from a client
			fatal_error(w, ERR_UNEXPECTED_MESSAGE, "sent an unframeable message");
			return;
		}
		w->last_rx = now_ms();
		handle_message(w, &m);
		if (!w->alive)
			return;
	}
}

static void *worker_main(void *arg)
{
	struct Worker w = {0};
	w.i = (int)(intptr_t)arg;
	w.opp = 1 - w.i;
	w.me = (u8)(CLIENT_1 + w.i);
	w.sock = slots[w.i].sock;
	w.alive = 1;
	w.last_rx = w.last_keepalive = now_ms();
	reader_init(&w.rd);

	for (;;) {
		struct pollfd pfd = {w.sock, POLLIN, 0};
		int ready = poll(&pfd, w.alive ? 1 : 0, POLL_MS);

		flush_outbox(&w);                       // 1. Flush the outbox
		check_game_end(&w);                     // 2. Check for the end of the game
		if (w.alive && ready > 0 && pfd.revents) // 3. Read messages
			read_messages(&w);
		if (!w.alive) {
			check_game_end(&w);
			continue;
		}

		long long now = now_ms();               // 4. Handle keepalives
		if (atomic_load_explicit(&slots[w.i].liveness, memory_order_relaxed) == SLOT_JOINED
		    && now - w.last_keepalive >= KEEPALIVE_INTERVAL_MS) {
			send_or_die(&w, MSG_KEEPALIVE, NULL, 0);
			w.last_keepalive = now;
		}
		if (now - w.last_rx >= KEEPALIVE_TIMEOUT_MS)
			client_dead(&w, "keepalive timeout");
		else if (reader_expired(&w.rd, now))
			client_dead(&w, "partial message timeout");
	}
	return NULL;
}

/* ---- Main thread ---- */

static void start_worker(int i, int sock)
{
	slots[i].sock = sock;
	memset(slots[i].id, 0, sizeof slots[i].id);
	atomic_store_explicit(&slots[i].pending, 0, memory_order_relaxed);
	atomic_store_explicit(&slots[i].liveness, SLOT_CONNECTED, memory_order_release);

	// Workers never receive SIGINT; only the main thread handles it
	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	if (pthread_create(&slots[i].thread, NULL, worker_main, (void *)(intptr_t)i) != 0) {
		perror("pthread_create");
		close(sock);
		atomic_store_explicit(&slots[i].liveness, SLOT_EMPTY, memory_order_relaxed);
	} else {
		slots[i].running = 1;
	}
	pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void stop_worker(int i)
{
	if (slots[i].running) {
		queue(i, PEND_DISCONNECT);
		pthread_join(slots[i].thread, NULL);
		slots[i].running = 0;
	}
	slots[i].id[0] = 0;
	atomic_store_explicit(&slots[i].pending, 0, memory_order_relaxed);
	atomic_store_explicit(&slots[i].liveness, SLOT_EMPTY, memory_order_release);
}

static u8 liveness(int i)
{
	return atomic_load_explicit(&slots[i].liveness, memory_order_acquire);
}

// Refused Connections: no worker owns this socket, so the main thread writes it.
static void refuse(int sock)
{
	struct Error e = {ERR_SERVER_FULL};
	if (send_msg_nowait(sock, MSG_ERROR, &e, sizeof e) == 0)
		send_msg_nowait(sock, MSG_DISCONNECT, NULL, 0);
	close(sock);
	log_msg("Refused a connection: server full");
}

// Accept one pending connection into an empty slot, or refuse it.
static void accept_one(int listener)
{
	int sock = accept(listener, NULL, NULL);
	if (sock < 0)
		return;
	for (int i = 0; i < 2; i++) {
		if (liveness(i) == SLOT_EMPTY) {
			start_worker(i, sock);
			return;
		}
	}
	refuse(sock);
}

static void note_exit_request(void)
{
	if (exit_requested && !exit_noted) {
		log_msg("Ctrl-C received: the server will exit when a game completes");
		exit_noted = 1;
	}
}

static int wait_listener(int listener)
{
	note_exit_request();
	struct pollfd pfd = {listener, POLLIN, 0};
	return poll(&pfd, 1, POLL_MS) > 0 && (pfd.revents & POLLIN);
}

// WAIT_P1 / WAIT_P2: admit connections until both slots have joined.
static void wait_for_players(int listener)
{
	log_msg("Waiting for players...");
	for (;;) {
		if (wait_listener(listener))
			accept_one(listener);
		for (int i = 0; i < 2; i++) {
			if (liveness(i) == SLOT_DEAD) {
				stop_worker(i);
				log_msg("A waiting client left");
			}
		}
		if (liveness(0) == SLOT_JOINED && liveness(1) == SLOT_JOINED)
			return;
	}
}

static void init_game(void)
{
	game_load_fen(&game, STANDARD_FEN);
	int active = rand() % 2;
	color_of[active] = WHITE;
	color_of[!active] = BLACK;
	clock_left[0] = clock_left[1] = CLOCK_DEFAULT_SECONDS;
	winner = TIE;
	atomic_store_explicit(&draw_state, 0, memory_order_relaxed);
	atomic_store_explicit(&config_tie, 0, memory_order_relaxed);

	for (int i = 0; i < 2; i++) {
		struct Start *s = &slots[i].outbox.start;
		memset(s, 0, sizeof *s);
		memcpy(s->id, slots[!i].id, ID_MAX_SIZE);
		s->color = color_of[i];
		game_to_start(&game, s);
	}
	log_msg("Game started: %s (white) vs %s (black)", slot_name(active), slot_name(!active));

	// Ownership is handed over before GO is queued
	atomic_store_explicit(&owner, (u8)(CLIENT_1 + active), memory_order_release);
	queue(!active, PEND_START);
	queue(active, PEND_START | PEND_GO);
}

static void finish_game(void)
{
	for (int i = 0; i < 2; i++) {
		struct End *e = &slots[i].outbox.end;
		e->victory = winner == TIE ? END_TIED : winner == i ? END_WON : END_LOST;
		be16_set(e->points, game.points[color_of[i]]);
		if (liveness(i) != SLOT_DEAD)
			queue(i, PEND_END);
	}
	if (winner == TIE)
		log_msg("Game over: draw");
	else
		log_msg("Game over: %s wins", slot_name(winner));
}

static int open_listener(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
	                           .sin_addr.s_addr = htonl(INADDR_ANY)};
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "[ERROR] Unable to bind port %d: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

static void usage(FILE *f)
{
	fprintf(f, "Usage: server -p PORT\n"
	           "  -p PORT  TCP port to listen on (1-65535), on all interfaces (0.0.0.0)\n"
	           "  -h       Show this help\n");
}

int main(int argc, char **argv)
{
	struct CliArgs args;
	switch (cli_parse(argc, argv, "p", &args)) {
	case CLI_HELP:
		usage(stdout);
		return 0;
	case CLI_ERROR:
		usage(stderr);
		return 1;
	case CLI_OK:
		break;
	}

	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {0};
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());

	int listener = open_listener(args.port);
	if (listener < 0)
		return 1;
	log_msg("Listening on 0.0.0.0:%d", args.port);

	for (;;) {
		atomic_store_explicit(&owner, MAIN, memory_order_release);
		wait_for_players(listener);
		init_game();

		// TURN / CLASSIFY run in the workers; keep refusing extra connections
		while (atomic_load_explicit(&owner, memory_order_acquire) != MAIN) {
			if (wait_listener(listener))
				accept_one(listener);
		}

		finish_game();                   // FINISH
		for (int i = 0; i < 2; i++)      // CLEANUP
			stop_worker(i);
		if (exit_requested) {
			log_msg("Exiting");
			close(listener);
			return 0;
		}
	}
}
