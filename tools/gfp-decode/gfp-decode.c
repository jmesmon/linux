
#include <stdio.h>
#include <stdlib.h>
#include "../../include/trace/events/gfpflags.h"

typedef unsigned gfp_t; /* from linux/types.h */
#define __force
#include "../../include/linux/gfp-flags.h"

/* taken from linux/ftrace_event.h */
struct trace_print_flags {
	unsigned long		mask;
	const char		*name;
};

/*
 * taken from include/trace/ftrace.h
 * modified to not pass 'p' to ftrace_print_flags_seq().
 */
#define __print_flags(flag, delim, flag_array...)			\
	({								\
		static const struct trace_print_flags __flags[] =	\
			{ flag_array, { -1, NULL } };			\
		ftrace_print_flags_seq(delim, flag, __flags);	\
	})

/*
 * taken from kernel/trace/trace_output.c
 * modified to use standard puts,putc, and printf and remove use of the buffer.
 */
static void
ftrace_print_flags_seq(const char *delim,
		       unsigned long flags,
		       const struct trace_print_flags *flag_array)
{
	unsigned long mask;
	const char *str;
	int i, first = 1;

	for (i = 0;  flag_array[i].name && flags; i++) {

		mask = flag_array[i].mask;
		if ((flags & mask) != mask)
			continue;

		str = flag_array[i].name;
		flags &= ~mask;
		if (!first && delim)
			fputs(delim, stdout);
		else
			first = 0;
		fputs(str, stdout);
	}

	/* check for left over flags */
	if (flags) {
		if (!first && delim)
			fputs(delim, stdout);
		printf("0x%lx", flags);
	}
	putchar('\n');
}

int main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		printf("usage: %s <gfp hex mask>..\n", argv[0]);
		return 1;
	}

	for (i = 1; i < argc; i++) {
		char *cur = argv[i];
		char *r;
		unsigned long flags;
		if (cur[0] == '0' && cur[1] == 'x')
			cur = &cur[2];
		flags = strtoul(cur, &r, 16);
		if (*r != '\0') {
			fprintf(stderr, "skipping arg %d\n", i-1);
			continue;
		}

		show_gfp_flags(flags);
	}

	return 0;
}
