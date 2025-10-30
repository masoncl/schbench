/*
 * options.c - Command line option parsing implementation
 */
#define _GNU_SOURCE
#include <sched.h>
#include <string.h>
#include <getopt.h>
#include "options.h"
#include "topology.h"

/* Global configuration variables - definitions */
int message_threads = 1;
int worker_threads = 0;
int runtime = 30;
int warmuptime = 0;
int intervaltime = 10;
int zerotime = 0;
unsigned long cache_footprint_kb = 256;
unsigned long operations = 5;
unsigned long sleep_usec = 100;
int auto_rps = 0;
int auto_rps_target_hit = 0;
int pipe_test = 0;
int requests_per_sec = 0;
int calibrate_only = 0;
int skip_locking = 0;
char *json_file = NULL;
char *jobname = NULL;
int pin_mode = PIN_MODE_NONE;
cpu_set_t *message_cpus = NULL;
cpu_set_t *worker_cpus = NULL;
struct cpu_topology topology = { 0 };
cpu_set_t *per_message_thread_cpus = NULL;

/* Static CPU sets */
static cpu_set_t __message_cpus = { 0 };
static cpu_set_t __worker_cpus = { 0 };

enum {
	HELP_LONG_OPT = 1,
};

char *option_string = "p:m:M:W:t:Cr:R:w:i:z:A:n:F:Lj:s:J:";
static struct option long_options[] = {
	{ "pipe", required_argument, 0, 'p' },
	{ "message-threads", required_argument, 0, 'm' },
	{ "message-cpus", required_argument, 0, 'M' },
	{ "worker-cpus", required_argument, 0, 'W' },
	{ "threads", required_argument, 0, 't' },
	{ "runtime", required_argument, 0, 'r' },
	{ "rps", required_argument, 0, 'R' },
	{ "auto-rps", required_argument, 0, 'A' },
	{ "cache_footprint", required_argument, 0, 'f' },
	{ "calibrate", no_argument, 0, 'C' },
	{ "no-locking", no_argument, 0, 'L' },
	{ "operations", required_argument, 0, 'n' },
	{ "sleep_usec", required_argument, 0, 's' },
	{ "warmuptime", required_argument, 0, 'w' },
	{ "intervaltime", required_argument, 0, 'i' },
	{ "zerotime", required_argument, 0, 'z' },
	{ "json", required_argument, 0, 'j' },
	{ "jobname", required_argument, 0, 'J' },
	{ "pin", required_argument, 0, 'P' },
	{ "help", no_argument, 0, HELP_LONG_OPT },
	{ 0, 0, 0, 0 }
};

void print_usage(void)
{
	fprintf(stderr,
		"schbench usage:\n"
		"\t-C (--calibrate): run our work loop and report on timing\n"
		"\t-L (--no-locking): don't spinlock during CPU work (def: locking on)\n"
		"\t-m (--message-threads): number of message threads (def: 1)\n"
		"\t-M (--message-cpus): pin message threads to these CPUs 'a-m,n-z' or 'auto' (def: no pinning)\n"
		"\t-W (--worker-cpus): pin worker threads to these CPUs 'a-m,n-z' or 'auto' (def: no pinning)\n"
		"\t-t (--threads): worker threads per message thread (def: num_cpus)\n"
		"\t-r (--runtime): How long to run before exiting (seconds, def: 30)\n"
		"\t-F (--cache_footprint): cache footprint (kb, def: 256)\n"
		"\t-n (--operations): think time operations to perform (def: 5)\n"
		"\t-s (--sleep_usec): think time sleep (usecs) per request\n"
		"\t-A (--auto-rps): grow RPS until cpu utilization hits target (def: none)\n"
		"\t-p (--pipe): transfer size bytes to simulate a pipe test (def: 0)\n"
		"\t-R (--rps): requests per second mode (count, def: 0)\n"
		"\t-w (--warmuptime): how long to warmup before resetting stats (seconds, def: 0)\n"
		"\t-i (--intervaltime): interval for printing latencies (seconds, def: 10)\n"
		"\t-z (--zerotime): interval for zeroing latencies (seconds, def: never)\n"
		"\t-j (--json) <file>: output in json format (def: false)\n"
		"\t-J (--jobname) <name>: an optional jobname to add to the json output (def: none)\n"
		"\t-P (--pin) ccx: pin threads to dies/chiplets (AMD CCX-aware pinning)\n");
	exit(1);
}

/*
 * returns 0 if we fail to parse and 1 if we succeed
 */
int parse_cpuset(const char *str, cpu_set_t *cpuset)
{
	char *input;
	CPU_ZERO(cpuset);
	if (!str || !*str)
		return 0; // Empty string is invalid

	input = strdup(str);
	if (!input)
		return 0;
	char *token = strtok(input, ",");
	while (token) {
		char *dash = strchr(token, '-');
		char *endptr;
		long start, end;

		if (dash) {
			*dash = '\0';
			dash++;
			errno = 0;
			start = strtol(token, &endptr, 10);
			if (errno || *endptr != '\0' || start < 0) {
				free(input);
				return 0;
			}
			errno = 0;
			end = strtol(dash, &endptr, 10);
			if (errno || *endptr != '\0' || end < start) {
				free(input);
				return 0;
			}
			for (long i = start; i <= end; ++i) {
				CPU_SET(i, cpuset);
			}
		} else {
			errno = 0;
			start = strtol(token, &endptr, 10);
			if (errno || *endptr != '\0' || start < 0) {
				free(input);
				return 0;
			}
			CPU_SET(start, cpuset);
		}
		token = strtok(NULL, ",");
	}
	free(input);
	return 1;
}

/*
 * -M and -W can take "auto", which means:
 *  give each message thread its own CPU
 *  give each worker thread all of the remaining CPUs
 */
static void thread_auto_pin(int m_threads, cpu_set_t *m_cpus, cpu_set_t *w_cpus)
{
	int i = 0;
	CPU_ZERO(m_cpus);
	for (i = 0; i < m_threads; ++i) {
		CPU_SET(i, m_cpus);
		CPU_CLR(i, w_cpus);
	}
	for (; i < CPU_SETSIZE; i++) {
		CPU_SET(i, w_cpus);
	}
	fprintf(stderr, "auto pinning message and worker threads\n");
}

void parse_options(int ac, char **av)
{
	int c;
	int found_warmuptime = -1;
	int found_auto_pin = 0;
	int i;

	while (1) {
		int option_index = 0;

		c = getopt_long(ac, av, option_string, long_options,
				&option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'C':
			calibrate_only = 1;
			break;
		case 'L':
			skip_locking = 1;
			break;
		case 'A':
			auto_rps = atoi(optarg);
			warmuptime = 0;
			if (requests_per_sec == 0)
				requests_per_sec = 10;
			break;
		case 'p':
			pipe_test = atoi(optarg);
			if (pipe_test > PIPE_TRANSFER_BUFFER) {
				fprintf(stderr, "pipe size too big, using %d\n",
					PIPE_TRANSFER_BUFFER);
				pipe_test = PIPE_TRANSFER_BUFFER;
			}
			warmuptime = 0;
			break;
		case 'w':
			found_warmuptime = atoi(optarg);
			break;
		case 'm':
			message_threads = atoi(optarg);
			break;
		case 'M':
			if (!strcmp(optarg, "auto")) {
				found_auto_pin = 1;
				pin_mode = PIN_MODE_AUTO;
			} else if (!parse_cpuset(optarg, &__message_cpus)) {
				fprintf(stderr,
					"failed to parse cpuset information\n");
				exit(1);
			} else {
				pin_mode = PIN_MODE_MANUAL;
			}
			message_cpus = &__message_cpus;
			break;
		case 'W':
			if (!strcmp(optarg, "auto")) {
				found_auto_pin = 1;
				pin_mode = PIN_MODE_AUTO;
			} else if (!parse_cpuset(optarg, &__worker_cpus)) {
				fprintf(stderr,
					"failed to parse cpuset information\n");
				exit(1);
			} else {
				pin_mode = PIN_MODE_MANUAL;
			}
			worker_cpus = &__worker_cpus;
			break;
		case 't':
			worker_threads = atoi(optarg);
			break;
		case 'r':
			runtime = atoi(optarg);
			break;
		case 'i':
			intervaltime = atoi(optarg);
			break;
		case 'z':
			zerotime = atoi(optarg);
			break;
		case 'R':
			requests_per_sec = atoi(optarg);
			break;
		case 'n':
			operations = atoi(optarg);
			break;
		case 's':
			sleep_usec = atoi(optarg);
			break;
		case 'F':
			cache_footprint_kb = atoi(optarg);
			break;
		case 'j':
			json_file = strdup(optarg);
			if (!json_file) {
				perror("strdup");
				exit(1);
			}
			break;
		case 'J':
			jobname = strdup(optarg);
			if (!jobname) {
				perror("strdup");
				exit(1);
			}
			break;
		case 'P':
			if (!strcmp(optarg, "ccx")) {
				pin_mode = PIN_MODE_CCX;
			} else {
				fprintf(stderr, "Unknown pin mode: %s\n",
					optarg);
				exit(1);
			}
			break;
		case '?':
		case HELP_LONG_OPT:
			print_usage();
			break;
		default:
			break;
		}
	}
	if (found_auto_pin) {
		thread_auto_pin(message_threads, &__message_cpus,
				&__worker_cpus);
		worker_cpus = &__worker_cpus;
		message_cpus = &__message_cpus;
	}

	/* Detect topology if using CCX pinning */
	if (pin_mode == PIN_MODE_CCX) {
		if (detect_topology(&topology) != 0) {
			fprintf(stderr, "Failed to detect CPU topology\n");
			exit(1);
		}
		print_topology(&topology);

		/* Allocate per-message-thread CPU sets */
		per_message_thread_cpus =
			calloc(message_threads, sizeof(cpu_set_t));
		if (!per_message_thread_cpus) {
			perror("Failed to allocate per-message-thread CPU sets");
			exit(1);
		}

		/* Assign message threads to dies in round-robin fashion */
		for (i = 0; i < message_threads; i++) {
			int die_id = i % topology.num_dies;
			memcpy(&per_message_thread_cpus[i],
			       &topology.dies[die_id].cpus, sizeof(cpu_set_t));
			print_thread_cpus("Message thread", i,
					  &per_message_thread_cpus[i]);
		}
	}
	/*
	 * by default pipe mode zeros out some options.  This
	 * sets them to any args that were actually passed in
	 */
	if (found_warmuptime >= 0)
		warmuptime = found_warmuptime;

	if (calibrate_only)
		skip_locking = 1;

	if (runtime < 30)
		warmuptime = 0;

	if (optind < ac) {
		fprintf(stderr, "Error Extra arguments '%s'\n", av[optind]);
		exit(1);
	}
}
