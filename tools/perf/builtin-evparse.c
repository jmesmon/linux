/*
 * builtin-evparse.c
 *
 * Test event parsing.
 *
 */
#include "builtin.h"

#include "perf.h"

#include "util/build-id.h"
#include "util/util.h"
#include "util/parse-options.h"
#include "util/parse-events.h"

#include "util/header.h"
#include "util/event.h"
#include "util/evlist.h"
#include "util/evsel.h"
#include "util/debug.h"
#include "util/session.h"
#include "util/tool.h"
#include "util/symbol.h"
#include "util/cpumap.h"
#include "util/thread_map.h"
#include "util/data.h"

#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>

struct evparse {
	struct record_opts	opts;
	struct perf_evlist	*evlist;
};

static const char * const evparse_usage[] = {
	"perf evparse [-e <event spec>] [-v]...",
	NULL
};

typedef unsigned long long ull;

static void show_perf_event_attr(struct perf_event_attr *attr, const char *indent, FILE *o)
{
	fprintf(o,
		"{\n"
		"%s	type = %llu,\n"
		"%s	config = %llx,\n"
		"%s	config1 = %llx,\n"
		"%s	config2 = %llx,\n"
		"%s}",
		indent, (ull)attr->type,
		indent, (ull)attr->config,
		indent, (ull)attr->config1,
		indent, (ull)attr->config2,
		indent);
}

static void show_evlist(struct perf_evlist *evlist, FILE *o)
{
	struct perf_evsel *evsel;
	fprintf(o, "event_count = %u\n", evlist->nr_entries);

	evlist__for_each(evlist, evsel) {
		{
			struct perf_attr_details details = { false, false, false };
			perf_evsel__fprintf(evsel, &details, o);
		}
		fprintf(o, "evsel %s {\n", perf_evsel__name(evsel));
		if (evsel->filter)
			fprintf(o, "\tfilter = \"%s\",\n", evsel->filter);
		fprintf(o, "\tscale = %f,\n", evsel->scale);
		fprintf(o, "\tunit  = \"%s\",\n", evsel->unit);
		fprintf(o, "\tattr  = ");

		show_perf_event_attr(&evsel->attr, "\t", o);
		fprintf(o, ",\n");
		perf_event_attr__fprintf(&evsel->attr, o);
		fprintf(o, "}\n");

	}
}

int cmd_evparse(int argc, const char **argv, const char *prefix __maybe_unused)
{
	int err = -ENOMEM;
	struct evparse evparse;
	const struct option evparse_options[] = {
		OPT_INCR('v', "verbose", &verbose, "be more verbose"),
		OPT_CALLBACK('e', "event", &evparse.evlist, "event",
				"event selector. use 'perf list' to list available events",
				parse_events_option),
		OPT_END()
	};


	evparse.evlist = perf_evlist__new();
	if (evparse.evlist == NULL)
		return -ENOMEM;

	argc = parse_options(argc, argv, evparse_options, evparse_usage,
			    PARSE_OPT_STOP_AT_NON_OPTION);
	if (!argc && target__none(&evparse.opts.target))
		usage_with_options(evparse_usage, evparse_options);

	if (evparse.evlist->nr_entries == 0 &&
	    perf_evlist__add_default(evparse.evlist) < 0) {
		pr_err("Not enough memory for event selector list\n");
		goto out;
	}

	printf("verbose = %d\n", verbose);
	show_evlist(evparse.evlist, stdout);
	return 0;
out:
	return err;
}
