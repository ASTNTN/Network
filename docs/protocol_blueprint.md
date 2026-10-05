# Application Protocol

## 2.1 Transport Layer & Packet Framing Mechanism

Each device keeps its state entirely local. Only changes in state are transmitted, and the server verifies all state changes.

- **Game**: (Fairy) chess
- **Language**: C
- **Transport Protocol**: TCP

### Framing Rule

Messages of a specific type have a predetermined size in bytes.

Reading messages takes two stages.

1. The message header byte is read from the TCP stream, identifying the message payload to follow. 
2. The message payload length is derived from the header type by lookup table, and the message payload is read from the stream.

In the event of a partial read, the reader must continue reading from the TCP stream until a full message has been accumulated. If the message is not completed by the time that the timeout expires, the connection is considered dead.

Multiple messages can be inbound in the TCP stream, but only one message will be read out from that stream at a time.

### Serialization Format

The message serialization format is a custom binary schema.

All messages are constructed from sequential reads of the following `struct Header` + payload pairs without padding. Because individual fields are constructed from octets, endianness is avoided entirely.

All message formats share a type header, but possess different fields.

```c
typedef unsigned char u8;

#define ID_MAX_SIZE 16
#define ID_MAX_LENGTH ID_MAX_SIZE - 1

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

// Empty structure - message is header-only
struct Accept { // Accept connection, server -> client
};

struct Start { // Start match, server -> client
	u8 id[ID_MAX_SIZE]; // Opponent ID as NUL-terminated string
	u8 game; // Board layout identifier
};

// Empty structure - message is header-only
struct Go { // Signal start of turn to active player, server -> client
};

struct Move { // Move request from client to server
	u8 start_x;
	u8 start_y;
	u8 end_x;
	u8 end_y;
};

struct Update { // State update from server to clients
	u8 start_x;
	u8 start_y;
	u8 end_x;
	u8 end_y;
};

struct End { // End of game, server -> client
	u8 victory; // As boolean. Whether the receiving client won or lost
};

// Empty structure - message is header-only
struct Disconnect { // Disconnect, bidirectional
};

const u8 PAYLOAD_SIZE[MSG_COUNT] = {
	[MSG_KEEPALIVE]  = sizeof(struct Keepalive),
	[MSG_ERROR]      = sizeof(struct Error),
	[MSG_CONNECT]    = sizeof(struct Connect),
	[MSG_ACCEPT]     = sizeof(struct Accept),
	[MSG_START]      = sizeof(struct Start),
	[MSG_GO]         = sizeof(struct Go),
	[MSG_MOVE]       = sizeof(struct Move),
	[MSG_UPDATE]     = sizeof(struct Update),
	[MSG_END]        = sizeof(struct End),
	[MSG_DISCONNECT] = sizeof(struct Disconnect)
};
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

## 2.2 Application Message Types

|    Type    |    Direction     | Purpose & Description                                                |
|:----------:|:----------------:|:---------------------------------------------------------------------|
| KEEPALIVE  |  bidirectional   | Peers send regular signals to confirm that the connection is live    |
|   ERROR    |  bidirectional   | Peers send errors for invalid moves and unexpected messages          |
|  CONNECT   | client -> server | Client requests to connect to server and peer in game under `id`     |
|   ACCEPT   | server -> client | Server accepts client request to connect                             |
|   START    | server -> client | Server notifies both clients that gameplay is beginning              |
|     GO     | server -> client | Server notifies one client that their turn is beginning              |
|    MOVE    | client -> server | Client submits requested move to server                              |
|   UPDATE   | server -> client | Server broadcasts approved updates to both clients                   |
|    END     | server -> client | Server notifies both clients that the game has concluded and who won |
| DISCONNECT |  bidirectional   | Peers send intentional termination of connection                     |

Intentional and unintentional disconnects by a client result in a forfeit.
