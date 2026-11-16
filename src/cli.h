/*
 * Command-line parsing shared by client and server (SOW.md §1.3).
 */
#ifndef CLI_H
#define CLI_H

struct CliArgs {
	const char *host; // -i, client only
	int port;         // -p
};

enum CliResult {
	CLI_OK,
	CLI_HELP,  // -h was given: print usage and exit 0
	CLI_ERROR  // A message naming the problem has been printed: print usage and exit 1
};

// Parse argv. `options` lists the required options other than -h:
// "p" for the server, "ip" for the client. Anything else is rejected.
enum CliResult cli_parse(int argc, char **argv, const char *options, struct CliArgs *out);

#endif
