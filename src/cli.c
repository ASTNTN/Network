#include "cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_port(const char *s, int *out)
{
	if (*s == '\0' || *s == '-' || *s == '+')
		return -1;
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || *end != '\0' || v < 1 || v > 65535)
		return -1;
	*out = (int)v;
	return 0;
}

enum CliResult cli_parse(int argc, char **argv, const char *options, struct CliArgs *out)
{
	int seen_i = 0, seen_p = 0;
	const char *port_text = NULL;
	out->host = NULL;
	out->port = 0;

	for (int a = 1; a < argc; a++) {
		const char *arg = argv[a];
		if (strcmp(arg, "-h") == 0)
			return CLI_HELP;
		if (arg[0] != '-' || arg[1] == '\0' || arg[2] != '\0' || !strchr(options, arg[1])) {
			if (arg[0] == '-')
				fprintf(stderr, "[ERROR] Unknown option '%s'.\n", arg);
			else
				fprintf(stderr, "[ERROR] Unexpected argument '%s'.\n", arg);
			return CLI_ERROR;
		}
		if (a + 1 >= argc) {
			fprintf(stderr, "[ERROR] Option '%s' needs a value.\n", arg);
			return CLI_ERROR;
		}
		const char *value = argv[++a];
		if (arg[1] == 'i') {
			if (seen_i++) {
				fprintf(stderr, "[ERROR] Option '-i' was given more than once.\n");
				return CLI_ERROR;
			}
			if (value[0] == '\0') {
				fprintf(stderr, "[ERROR] The server hostname given with '-i' is empty.\n");
				return CLI_ERROR;
			}
			out->host = value;
		} else {
			if (seen_p++) {
				fprintf(stderr, "[ERROR] Option '-p' was given more than once.\n");
				return CLI_ERROR;
			}
			port_text = value;
		}
	}

	if (strchr(options, 'i') && !seen_i) {
		fprintf(stderr, "[ERROR] Missing required option '-i HOSTNAME'.\n");
		return CLI_ERROR;
	}
	if (!seen_p) {
		fprintf(stderr, "[ERROR] Missing required option '-p PORT'.\n");
		return CLI_ERROR;
	}
	if (parse_port(port_text, &out->port) < 0) {
		fprintf(stderr, "[ERROR] Invalid port '%s': expected a number from 1 to 65535.\n", port_text);
		return CLI_ERROR;
	}
	return CLI_OK;
}
