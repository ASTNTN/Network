/*
 * Fairy Chess client.
 *
 * Implements the client FSM (docs/fsm_specification.md): ENTER, PAIR,
 * WAIT, TURN, FINISH. A single thread polls the server socket and the
 * keyboard, so keepalives and server messages keep flowing while the user
 * is typing.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli.h"
#include "engine.h"
#include "net.h"
#include "timing.h"

enum State { PAIR, WAIT, TURN };

struct Client {
	int fd;
	enum State st;
	struct Reader rd;
	long long last_rx, last_keepalive;

	u8 id[ID_MAX_SIZE];
	int awaiting_id;   // PAIR: the server rejected our ID; the next line is a new one

	struct Game g;
	u8 color;
	char opponent[ID_MAX_SIZE];
	int awaiting_move; // A GO arrived and no MOVE has been sent yet
	int offer_pending; // The opponent's draw offer for the current turn
};

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
	(void)sig;
	interrupted = 1;
}

/* ---- Keyboard input, read a line at a time without blocking ---- */

static char inbuf[256];
static size_t inlen;

// Read what stdin has. Returns -1 at end of input.
static int fill_stdin(void)
{
	ssize_t n = read(STDIN_FILENO, inbuf + inlen, sizeof inbuf - 1 - inlen);
	if (n < 0)
		return errno == EINTR || errno == EAGAIN ? 0 : -1;
	if (n == 0)
		return -1;
	inlen += (size_t)n;
	return 0;
}

// Take the next complete line, without its newline and surrounding spaces.
static int next_line(char *out, size_t cap)
{
	char *nl = memchr(inbuf, '\n', inlen);
	size_t take = nl ? (size_t)(nl - inbuf) + 1 : inlen;
	if (!nl && inlen < sizeof inbuf - 1)
		return 0; // Incomplete line (an overlong one is taken as it is)

	size_t len = nl ? take - 1 : take;
	char *start = inbuf;
	while (len && isspace((unsigned char)*start)) {
		start++;
		len--;
	}
	while (len && isspace((unsigned char)start[len - 1]))
		len--;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, start, len);
	out[len] = '\0';

	memmove(inbuf, inbuf + take, inlen - take);
	inlen -= take;
	return 1;
}

static void prompt(const char *text)
{
	fputs(text, stdout);
	fflush(stdout);
}

/* ---- Sending ---- */

static void lost_server(const char *why)
{
	printf("\n[ERROR] %s\n", why);
	exit(1);
}

static void send_or_exit(struct Client *c, u8 type, const void *payload, size_t len)
{
	if (send_msg(c->fd, type, payload, len) < 0)
		lost_server("Lost the connection to the server.");
}

static void send_error(struct Client *c, u8 code)
{
	struct Error e = {code};
	send_or_exit(c, MSG_ERROR, &e, sizeof e);
}

// Fatal error: ERROR, then DISCONNECT, then exit.
static void fatal(struct Client *c, u8 code, const char *why)
{
	printf("\n[ERROR] %s (%s). Disconnecting.\n", why, error_str(code));
	struct Error e = {code};
	send_msg(c->fd, MSG_ERROR, &e, sizeof e);
	send_msg(c->fd, MSG_DISCONNECT, NULL, 0);
	close(c->fd);
	exit(1);
}

// Leaving is always a forfeit during a game.
static void quit(struct Client *c)
{
	send_msg(c->fd, MSG_DISCONNECT, NULL, 0);
	close(c->fd);
	if (c->st == TURN)
		printf("\nYou left the game. Leaving counts as a forfeit.\n");
	else
		printf("\nDisconnected.\n");
	exit(0);
}

static void send_connect(struct Client *c)
{
	struct Connect m;
	memcpy(m.id, c->id, sizeof m.id);
	send_or_exit(c, MSG_CONNECT, &m, sizeof m);
}

/* ---- Display ---- */

static const char *color_name(u8 color)
{
	return color == WHITE ? "White" : "Black";
}

static void show_board(const struct Client *c)
{
	putchar('\n');
	game_print(&c->g, stdout, c->color);
	printf("Material points: you %u, %s %u\n", c->g.points[c->color], c->opponent,
	       c->g.points[!c->color]);
}

static void show_help(void)
{
	printf("Commands: a move such as e2e4 (e7e8q to promote, e1g1 to castle), "
	       "'draw' to offer or accept a draw, 'quit' to leave (a forfeit).\n");
}

/* ---- Server messages ---- */

static void handle_start(struct Client *c, const struct Msg *m)
{
	struct Start s = {0};
	memcpy(&s, m->payload, m->len);
	u8 code = game_from_start(&c->g, &s);
	if (code)
		fatal(c, code, "The server sent an invalid board layout");

	c->color = s.color;
	memcpy(c->opponent, s.id, ID_MAX_SIZE);
	c->opponent[ID_MAX_SIZE - 1] = '\0';
	c->st = TURN;
	printf("\nThe game has started against %s. You play %s.\n", c->opponent, color_name(c->color));
	show_board(c);
	if (c->color != WHITE)
		printf("Waiting for %s's move...\n", c->opponent);
	fflush(stdout);
}

static void handle_go(struct Client *c, const struct Msg *m)
{
	struct Go go;
	memcpy(&go, m->payload, sizeof go);
	unsigned t = be16_get(go.time);
	if (t == 0) {
		printf("[WARNING] The server started your turn with no time left.\n");
		send_error(c, ERR_ZERO_TIME);
		return;
	}
	c->awaiting_move = 1;
	printf("\n>>> Your turn (%u:%02u left on your clock)\n", t / 60, t % 60);
	if (c->offer_pending)
		printf("%s has offered a draw; type 'draw' to accept.\n", c->opponent);
	prompt("Your move: ");
}

static void handle_update(struct Client *c, const struct Msg *m)
{
	struct Update up;
	memcpy(&up, m->payload, sizeof up);
	struct Move mv;
	memcpy(&mv, &up, sizeof mv);

	char text[MOVE_TEXT_MAX];
	move_format(&c->g, &mv, text);
	u8 mover = c->g.turn;
	enum MoveResult r = game_move(&c->g, &mv);
	if (MOVE_IS_INVALID(r))
		fatal(c, ERR_UPDATE_BAD_STATE, "Could not apply the server's update");

	c->offer_pending = 0; // Offers lapse when the turn ends
	printf("\n%s (%s) played %s.\n", color_name(mover),
	       mover == c->color ? "you" : c->opponent, text);
	show_board(c);
	if (r == MOVE_VICTORY)
		printf("Checkmate!\n");
	else if (r == MOVE_DRAW)
		printf("The position is drawn.\n");
	else if (mover == c->color)
		printf("Waiting for %s's move...\n", c->opponent);
	fflush(stdout);
}

static void handle_draw(struct Client *c, const struct Msg *m)
{
	struct Draw d;
	memcpy(&d, m->payload, sizeof d);
	if (be16_get(d.turn) != (c->g.ply & 0xFFFF))
		return; // An offer for another turn
	c->offer_pending = 1;
	printf("\n%s offers a draw. Type 'draw' to accept, or play on.\n", c->opponent);
	if (c->awaiting_move)
		prompt("Your move: ");
}

static void handle_error(struct Client *c, const struct Msg *m)
{
	u8 code = m->payload[0];
	printf("\n[SERVER ERROR] %s\n", error_str(code));
	if (c->st == PAIR && (code == ERR_BAD_PLAYER_ID || code == ERR_DUPLICATE_PLAYER_ID)) {
		c->awaiting_id = 1;
		prompt("Enter a different player ID: ");
	}
	fflush(stdout);
}

// FINISH: show the result and score summary, then leave.
static void handle_end(struct Client *c, const struct Msg *m)
{
	struct End e;
	memcpy(&e, m->payload, sizeof e);
	const char *result = e.victory == END_WON ? "You won!"
	                   : e.victory == END_LOST ? "You lost." : "The game is a draw.";
	printf("\n=== Game over: %s ===\n", result);
	printf("Score summary: %s Your material points: %u.",
	       e.victory == END_WON ? "Win." : e.victory == END_LOST ? "Loss." : "Draw.",
	       be16_get(e.points));
	if (c->st == TURN)
		printf(" %s's material points: %u.", c->opponent, c->g.points[!c->color]);
	printf("\n");
	send_msg(c->fd, MSG_DISCONNECT, NULL, 0);
	close(c->fd);
	exit(0);
}

static void handle_message(struct Client *c, const struct Msg *m)
{
	switch (m->type) {
	case MSG_KEEPALIVE:
		return;
	case MSG_ERROR:
		handle_error(c, m);
		return;
	case MSG_DISCONNECT:
		lost_server("The server closed the connection.");
		return;
	case MSG_DRAW:
		if (c->st == TURN)
			handle_draw(c, m);
		return; // Ignored in PAIR and WAIT
	case MSG_END:
		if (c->st != PAIR) {
			handle_end(c, m);
			return;
		}
		break;
	case MSG_ACCEPT:
		if (c->st == PAIR) {
			c->st = WAIT;
			printf("\nConnected as %s. Waiting for Player 2...\n", (const char *)c->id);
			fflush(stdout);
			return;
		}
		break;
	case MSG_START:
		if (c->st == WAIT) {
			handle_start(c, m);
			return;
		}
		break;
	case MSG_GO:
		if (c->st == TURN) {
			handle_go(c, m);
			return;
		}
		break;
	case MSG_UPDATE:
		if (c->st == TURN) {
			handle_update(c, m);
			return;
		}
		break;
	}
	send_error(c, ERR_UNEXPECTED_MESSAGE);
}

static void read_server(struct Client *c)
{
	if (reader_fill(&c->rd, c->fd) < 0)
		lost_server("The connection to the server was closed.");
	struct Msg m;
	for (;;) {
		switch (reader_next(&c->rd, &m)) {
		case READ_NONE:
			return;
		case READ_BAD_TYPE:
			fatal(c, ERR_UNKNOWN_TYPE, "The server sent an unknown message type");
			return;
		case READ_BAD_LENGTH:
			if (c->st == WAIT)
				fatal(c, ERR_CONFIG_COUNT, "The server sent an invalid board layout");
			fatal(c, ERR_UNEXPECTED_MESSAGE, "The server sent an unreadable message");
			return;
		case READ_MSG:
			c->last_rx = now_ms();
			handle_message(c, &m);
		}
	}
}

/* ---- User commands ---- */

static void handle_line(struct Client *c, char *line)
{
	if (line[0] == '\0')
		return;
	if (strcmp(line, "quit") == 0)
		quit(c);

	if (c->awaiting_id) {
		u8 id[ID_MAX_SIZE] = {0};
		if (strlen(line) > ID_MAX_LENGTH || (strncpy((char *)id, line, ID_MAX_LENGTH), !player_id_valid(id))) {
			prompt("IDs must be 1-15 printable ASCII characters. Enter a different player ID: ");
			return;
		}
		memcpy(c->id, id, sizeof id);
		c->awaiting_id = 0;
		send_connect(c);
		return;
	}

	if (strcmp(line, "help") == 0) {
		show_help();
	} else if (strcmp(line, "draw") == 0) {
		if (c->st != TURN) {
			printf("No game is in progress.\n");
			return;
		}
		struct Draw d;
		be16_set(d.turn, c->g.ply & 0xFFFF);
		send_or_exit(c, MSG_DRAW, &d, sizeof d);
		printf(c->offer_pending ? "You accepted the draw offer.\n"
		                        : "Draw offered for this turn.\n");
	} else if (c->st != TURN) {
		printf("The game hasn't started yet. (Type 'quit' to leave.)\n");
	} else if (!c->awaiting_move) {
		printf("It's not your turn yet.\n");
	} else {
		struct Move mv;
		if (move_parse(line, &mv) < 0) {
			printf("Couldn't read '%s'. ", line);
			show_help();
			prompt("Your move: ");
			return;
		}
		game_fill_piece(&c->g, &mv);
		send_or_exit(c, MSG_MOVE, &mv, sizeof mv);
		c->awaiting_move = 0;
	}
	fflush(stdout);
}

/* ---- ENTER ---- */

// Read the player ID before connecting, blocking on the keyboard.
static void ask_id(u8 id[ID_MAX_SIZE])
{
	char line[128];
	prompt("Enter your player ID (1-15 printable characters): ");
	for (;;) {
		while (!next_line(line, sizeof line)) {
			struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
			if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
				exit(1);
			if (interrupted)
				exit(0);
			if (pfd.revents && fill_stdin() < 0) {
				printf("\n");
				exit(0);
			}
		}
		memset(id, 0, ID_MAX_SIZE);
		if (strlen(line) <= ID_MAX_LENGTH) {
			strcpy((char *)id, line);
			if (player_id_valid(id))
				return;
		}
		prompt("IDs must be 1-15 printable ASCII characters. Enter your player ID: ");
	}
}

// Resolve the server's hostname (Requirement.md §2.2): exit cleanly with status 1 on failure.
static struct sockaddr_in resolve_server(const char *host, int port)
{
	struct addrinfo hints = {0}, *res;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0) {
		fprintf(stderr, "[ERROR] Unable to resolve DNS hostname '%s': %s.\n", host,
		        rc == EAI_SYSTEM ? strerror(errno) : gai_strerror(rc));
		fprintf(stderr, "Please check your Cisco router DNS configuration ('ip dns server' / "
		                "'ip host') or verify domain spelling.\n");
		exit(1);
	}
	struct sockaddr_in addr;
	memcpy(&addr, res->ai_addr, sizeof addr);
	freeaddrinfo(res);
	addr.sin_port = htons((uint16_t)port);
	return addr;
}

static int connect_server(const char *host, const struct sockaddr_in *addr)
{
	char ip[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof ip);
	int port = ntohs(addr->sin_port);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[ERROR] Unable to create a socket: %s.\n", strerror(errno));
		exit(1);
	}
	if (connect(fd, (const struct sockaddr *)addr, sizeof *addr) < 0) {
		fprintf(stderr, "[ERROR] Unable to connect to %s (%s) on port %d: %s.\n", host, ip, port,
		        strerror(errno));
		exit(1);
	}
	printf("Connected to %s (%s) on port %d.\n", host, ip, port);
	return fd;
}

static void usage(FILE *f)
{
	fprintf(f, "Usage: client -i HOSTNAME -p PORT\n"
	           "  -i HOSTNAME  Server DNS hostname (or IPv4 address)\n"
	           "  -p PORT      Server TCP port (1-65535)\n"
	           "  -h           Show this help\n");
}

int main(int argc, char **argv)
{
	struct CliArgs args;
	switch (cli_parse(argc, argv, "ip", &args)) {
	case CLI_HELP:
		usage(stdout);
		return 0;
	case CLI_ERROR:
		usage(stderr);
		return 1;
	case CLI_OK:
		break;
	}

	setvbuf(stdout, NULL, _IOLBF, 0); // Show every line at once, even through a pipe
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {0};
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	// ENTER: resolve first, so a DNS failure is reported before anything else
	static struct Client c;
	struct sockaddr_in addr = resolve_server(args.host, args.port);
	ask_id(c.id);
	c.fd = connect_server(args.host, &addr);
	c.st = PAIR;
	reader_init(&c.rd);
	c.last_rx = c.last_keepalive = now_ms();
	send_connect(&c);

	for (;;) {
		if (interrupted)
			quit(&c);

		struct pollfd pfd[2] = {{c.fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
		int n = poll(pfd, 2, POLL_MS);
		if (n < 0 && errno != EINTR)
			lost_server("poll failed.");

		if (n > 0 && pfd[0].revents)
			read_server(&c);
		if (n > 0 && pfd[1].revents) {
			if (fill_stdin() < 0)
				quit(&c); // End of input: the user has left
			char line[128];
			while (next_line(line, sizeof line))
				handle_line(&c, line);
		}

		long long now = now_ms();
		if (now - c.last_keepalive >= KEEPALIVE_INTERVAL_MS) {
			send_or_exit(&c, MSG_KEEPALIVE, NULL, 0);
			c.last_keepalive = now;
		}
		if (now - c.last_rx >= KEEPALIVE_TIMEOUT_MS)
			lost_server("Lost contact with the server (keepalive timeout).");
		if (reader_expired(&c.rd, now))
			lost_server("The server sent an incomplete message (timeout).");
	}
}
