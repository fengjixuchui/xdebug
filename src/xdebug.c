// Copyright 2023, Brian Swetland <swetland@frotz.net>
// Licensed under the Apache License, Version 2.0.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

#include "xdebug.h"
#include "tui.h"

#include "transport.h"

#define MAX_ARGS 16

static const char* NTH(unsigned n) {
	switch (n) {
	case 1: return "1st ";
	case 2: return "2nd ";
	case 3: return "3rd ";
	case 4: return "4th ";
	case 5: return "5th ";
	default: return "";
	}
}

#define tSTRING 0x01
#define tNUMBER 0x02

typedef struct token {
	const char* s;
	uint32_t n;
	uint32_t info;
} TOKEN;

struct command_context {
	TOKEN tok[MAX_ARGS];
	unsigned count;
};

const char* cmd_name(CC* cc) {
	return cc->tok[0].s;
}

int cmd_arg_u32(CC* cc, unsigned nth, uint32_t* out) {
	if (nth >= cc->count) {
		ERROR("%s: missing %sargument\n", cc->tok[0].s, NTH(nth));
		return DBG_ERR;
	}
	if (!(cc->tok[nth].info & tNUMBER)) {
		ERROR("%s: %sargument not a number\n", cc->tok[0].s, NTH(nth));
		return DBG_ERR;
	}
	*out = cc->tok[nth].n;
	return 0;
}

int cmd_arg_u32_opt(CC* cc, unsigned nth, uint32_t* out, uint32_t val) {
	if (nth >= cc->count) {
		*out = val;
		return 0;
	}
	if (!(cc->tok[nth].info & tNUMBER)) {
		ERROR("%s: %sargument not a number\n", cc->tok[0].s, NTH(nth));
		return DBG_ERR;
	}
	*out = cc->tok[nth].n;
	return 0;
}

int cmd_arg_str(CC* cc, unsigned nth, const char** out) {
	if (nth >= cc->count) {
		ERROR("%s: missing %sargument\n", cc->tok[0].s, NTH(nth));
		return DBG_ERR;
	}
	*out = cc->tok[nth].s;
	return 0;
}

int cmd_arg_str_opt(CC* cc, unsigned nth, const char** out, const char* str) {
	if (nth >= cc->count) {
		*out = str;
	} else {
		*out = cc->tok[nth].s;
	}
	return 0;
}

static DC* dc;

int parse(TOKEN* tok) {
	char *end;
	if ((tok->s[0] == '.') && tok->s[1]) {
		// decimal
		tok->n = strtoul(tok->s + 1, &end, 10);
		if (*end == 0) {
			tok->info = tNUMBER;
			return 0;
		}
	}
	tok->n = strtoul(tok->s, &end, 16);
	if (*end == 0) {
		tok->info = tNUMBER;
		return 0;
	}
	tok->n = 0;
	tok->info = tSTRING;
	return 0;
}

void handle_line(char *line, unsigned len) {
	CC cc;

	while (*line && (*line <= ' ')) line++;
	if (*line == '/') {
		cc.count = 2;
		cc.tok[0].s = "wconsole";
		cc.tok[0].info = tSTRING;
		cc.tok[1].s = line + 1;
		cc.tok[1].info = tSTRING;
		cc.count = 2;
		debugger_command(dc, &cc);
		return;
	}

	unsigned c, n = 0;
	while ((c = *line)) {
		if (c <= ' ') {
			line++;
			continue;
		}
		if (n == MAX_ARGS) {
			ERROR("too many arguments\n");
			return;
		}
		cc.tok[n].s = line;
		for (;;) {
			if (c == 0) {
				n++;
				break;
			} else if (c == '#') {
				*line = 0;
				break;
			} else if (c <= ' ') {
				*line++ = 0;
				n++;
				break;
			}
			c = *++line;
		}
	}

	if (n == 0) {
		return;
	}

	cc.tok[0].info = tSTRING;
	for (c = 1; c < n; c++) {
		if (parse(cc.tok + c) < 0) {
			return;
		}	
	}
	cc.count = n;
	debugger_command(dc, &cc);
}

static tui_ch_t* ch;

int main(int argc, char** argv) {
	tui_init();
	tui_ch_create(&ch, 0);
	dc_create(&dc);
	while (tui_handle_event(handle_line) == 0) ;
	tui_exit();
	return 0;
}

void debugger_exit(void) {
	tui_exit();
	exit(0);
}

void MSG(uint32_t flags, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	switch (flags) {
	case mDEBUG:
		tui_ch_printf(ch, "debug: ");
		break;
	case mTRACE:
		tui_ch_printf(ch, "trace: ");
		break;
	case mPANIC:
		tui_exit();
		fprintf(stderr,"panic: ");
		vfprintf(stderr, fmt, ap);
		exit(-1);
	}
	tui_ch_vprintf(ch, fmt, ap);
	va_end(ap);
}
