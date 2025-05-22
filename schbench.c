/*
 * schbench.c
 *
 * Copyright (C) 2016 Facebook
 * Chris Mason <clm@fb.com>
 *
 * GPLv2, portions copied from the kernel and from Jens Axboe's fio
 *
 * gcc -Wall -O0 -W schbench.c -o schbench -lpthread
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <linux/futex.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <netdb.h>

#define PLAT_BITS	8
#define PLAT_VAL	(1 << PLAT_BITS)
#define PLAT_GROUP_NR	19
#define PLAT_NR		(PLAT_GROUP_NR * PLAT_VAL)
#define PLAT_LIST_MAX	20

/* when -p is on, how much do we send back and forth */
#define PIPE_TRANSFER_BUFFER (1 * 1024 * 1024)

#define USEC_PER_SEC (1000000)

/* -m number of message threads */
static int message_threads = 1;
/* -t  number of workers per message thread */
static int worker_threads = 0;
/* -r  seconds */
static int runtime = 30;
/* -w  seconds */
static int warmuptime = 0;
/* -i  seconds */
static int intervaltime = 10;
/* -z  seconds */
static int zerotime = 0;
/* -f  cache_footprint_kb */
static unsigned long cache_footprint_kb = 256;
/* -n  operations */
static unsigned long operations = 5;
/* -s  sleep_usec */
static unsigned long sleep_usec = 100;
/* -A, int percentage busy */
static int auto_rps = 0;
static int auto_rps_target_hit = 0;
/* -p bytes */
static int pipe_test = 0;
/* -R requests per sec */
static int requests_per_sec = 0;
/* -C bool for calibration mode */
static int calibrate_only = 0;
/* -L bool no locking during CPU work */
static int skip_locking = 0;
/* -j json file */
static char *json_file = NULL;
/* -J jobname */
static char *jobname = NULL;

/* the message threads flip this to true when they decide runtime is up */
static volatile unsigned long stopping = 0;

/* size of matrices to multiply */
static unsigned long matrix_size = 0;

struct per_cpu_lock {
	pthread_mutex_t lock;
} __attribute__((aligned));

static struct per_cpu_lock *per_cpu_locks;
static int num_cpu_locks;

/* for pinning threads to CPUs */
static cpu_set_t *message_cpus = NULL;
static cpu_set_t __message_cpus = { 0 };
static cpu_set_t *worker_cpus = NULL;
static cpu_set_t __worker_cpus = { 0 };

/*
 * one stat struct per thread data, when the workers sleep this records the
 * latency between when they are woken up and when they actually get the
 * CPU again.  The message threads sum up the stats of all the workers and
 * then bubble them up to main() for printing
 */
struct stats {
	unsigned int plat[PLAT_NR];
	unsigned long nr_samples;
	unsigned int max;
	unsigned int min;
};

struct stats rps_stats;

/* this defines which latency profiles get printed */
#define PLIST_20 (1 << 0)
#define PLIST_50 (1 << 1)
#define PLIST_90 (1 << 2)
#define PLIST_99 (1 << 3)
#define PLIST_99_INDEX 3
#define PLIST_999 (1 << 4)

#define PLIST_FOR_LAT (PLIST_50 | PLIST_90 | PLIST_99 | PLIST_999)
#define PLIST_FOR_RPS (PLIST_20 | PLIST_50 | PLIST_90)

static double plist[PLAT_LIST_MAX] = { 20.0, 50.0, 90.0, 99.0, 99.9 };

enum {
	HELP_LONG_OPT = 1,
};

char *option_string = "p:m:M:W:t:Cr:R:w:i:z:A:n:F:Lj:s:J:";
static struct option long_options[] = {
	{"pipe", required_argument, 0, 'p'},
	{"message-threads", required_argument, 0, 'm'},
	{"message-cpus", required_argument, 0, 'M'},
	{"worker-cpus", required_argument, 0, 'W'},
	{"threads", required_argument, 0, 't'},
	{"runtime", required_argument, 0, 'r'},
	{"rps", required_argument, 0, 'R'},
	{"auto-rps", required_argument, 0, 'A'},
	{"cache_footprint", required_argument, 0, 'f'},
	{"calibrate", no_argument, 0, 'C'},
	{"no-locking", no_argument, 0, 'L'},
	{"operations", required_argument, 0, 'n'},
	{"sleep_usec", required_argument, 0, 'n'},
	{"warmuptime", required_argument, 0, 'w'},
	{"intervaltime", required_argument, 0, 'i'},
	{"zerotime", required_argument, 0, 'z'},
	{"json", required_argument, 0, 'j'},
	{"jobname", required_argument, 0, 'J'},
	{"help", no_argument, 0, HELP_LONG_OPT},
	{0, 0, 0, 0}
};

static void print_usage(void)
{
	fprintf(stderr, "schbench usage:\n"
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
	       );
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
static void thread_auto_pin(int m_threads, cpu_set_t *m_cpus,
				     cpu_set_t *w_cpus)
{
	int i = 0;
	CPU_ZERO(m_cpus);
	for (int i = 0; i < m_threads; ++i) {
		CPU_SET(i, m_cpus);
		CPU_CLR(i, w_cpus);
	}
	for (; i < CPU_SETSIZE; i++) {
		CPU_SET(i, w_cpus);
	}
	fprintf(stderr, "auto pinning message and worker threads\n");
}

static void parse_options(int ac, char **av)
{
	int c;
	int found_warmuptime = -1;
	int found_auto_pin = 0;

	while (1) {
		int option_index = 0;

		c = getopt_long(ac, av, option_string,
				long_options, &option_index);

		if (c == -1)
			break;

		switch(c) {
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
			} else if (!parse_cpuset(optarg, &__message_cpus)) {
				fprintf(stderr, "failed to parse cpuset information\n");
				exit(1);
			}
			message_cpus = &__message_cpus;
			break;
		case 'W':
			if (!strcmp(optarg, "auto")) {
				found_auto_pin = 1;
			} else if (!parse_cpuset(optarg, &__worker_cpus)) {
				fprintf(stderr, "failed to parse cpuset information\n");
				exit(1);
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
				perror("stdrup");
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
		thread_auto_pin(message_threads,
		  &__message_cpus, &__worker_cpus);
		worker_cpus = &__worker_cpus;
		message_cpus = &__message_cpus;
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

void tvsub(struct timeval * tdiff, struct timeval * t1, struct timeval * t0)
{
	tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
	tdiff->tv_usec = t1->tv_usec - t0->tv_usec;
	if (tdiff->tv_usec < 0 && tdiff->tv_sec > 0) {
		tdiff->tv_sec--;
		tdiff->tv_usec += USEC_PER_SEC;
		if (tdiff->tv_usec < 0) {
			fprintf(stderr, "lat_fs: tvsub shows test time ran backwards!\n");
			exit(1);
		}
	}

	/* time shouldn't go backwards!!! */
	if (tdiff->tv_usec < 0 || t1->tv_sec < t0->tv_sec) {
		tdiff->tv_sec = 0;
		tdiff->tv_usec = 0;
	}
}

/*
 * returns the difference between start and stop in usecs.  Negative values
 * are turned into 0
 */
unsigned long long tvdelta(struct timeval *start, struct timeval *stop)
{
	struct timeval td;
	unsigned long long usecs;

	tvsub(&td, stop, start);
	usecs = td.tv_sec;
	usecs *= USEC_PER_SEC;
	usecs += td.tv_usec;
	return (usecs);
}

/* mr axboe's magic latency histogram */
static unsigned int plat_val_to_idx(unsigned int val)
{
	unsigned int msb, error_bits, base, offset;

	/* Find MSB starting from bit 0 */
	if (val == 0)
		msb = 0;
	else
		msb = sizeof(val)*8 - __builtin_clz(val) - 1;

	/*
	 * MSB <= (PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index
	 */
	if (msb <= PLAT_BITS)
		return val;

	/* Compute the number of error bits to discard*/
	error_bits = msb - PLAT_BITS;

	/* Compute the number of buckets before the group */
	base = (error_bits + 1) << PLAT_BITS;

	/*
	 * Discard the error bits and apply the mask to find the
	 * index for the buckets in the group
	 */
	offset = (PLAT_VAL - 1) & (val >> error_bits);

	/* Make sure the index does not exceed (array size - 1) */
	return (base + offset) < (PLAT_NR - 1) ?
		(base + offset) : (PLAT_NR - 1);
}

/*
 * Convert the given index of the bucket array to the value
 * represented by the bucket
 */
static unsigned int plat_idx_to_val(unsigned int idx)
{
	unsigned int error_bits, k, base;

	if (idx >= PLAT_NR) {
		fprintf(stderr, "idx %u is too large\n", idx);
		exit(1);
	}

	/* MSB <= (PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index */
	if (idx < (PLAT_VAL << 1))
		return idx;

	/* Find the group and compute the minimum value of that group */
	error_bits = (idx >> PLAT_BITS) - 1;
	base = 1 << (error_bits + PLAT_BITS);

	/* Find its bucket number of the group */
	k = idx % PLAT_VAL;

	/* Return the mean of the range of the bucket */
	return base + ((k + 0.5) * (1 << error_bits));
}


static unsigned int calc_percentiles(unsigned int *io_u_plat, unsigned long nr,
				     unsigned int **output,
				     unsigned long **output_counts)
{
	unsigned long sum = 0;
	unsigned int len, i, j = 0;
	unsigned int oval_len = 0;
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned long last = 0;
	int is_last;

	len = 0;
	while (len < PLAT_LIST_MAX && plist[len] != 0.0)
		len++;

	if (!len)
		return 0;

	/*
	 * Calculate bucket values, note down max and min values
	 */
	is_last = 0;
	for (i = 0; i < PLAT_NR && !is_last; i++) {
		sum += io_u_plat[i];
		while (sum >= (plist[j] / 100.0 * nr)) {
			if (j == oval_len) {
				oval_len += 100;
				ovals = realloc(ovals, oval_len * sizeof(unsigned int));
				ocounts = realloc(ocounts, oval_len * sizeof(unsigned long));
			}

			ovals[j] = plat_idx_to_val(i);
			ocounts[j] = sum;
			is_last = (j == len - 1);
			if (is_last)
				break;
			j++;
		}
	}

	for (i = 1; i < len; i++) {
		last += ocounts[i - 1];
		ocounts[i] -= last;
	}
	*output = ovals;
	*output_counts = ocounts;
	return len;
}

static void show_latencies(struct stats *s, char *label, char *units,
			   unsigned long long runtime, unsigned long mask,
			   unsigned long star)
{
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned int len, i;

	len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
	if (len) {
		fprintf(stderr, "%s percentiles (%s) runtime %llu (s) (%lu total samples)\n",
			label, units, runtime, s->nr_samples);
		for (i = 0; i < len; i++) {
			unsigned long bit = 1 << i;
			if (!(mask & bit))
				continue;
			fprintf(stderr, "\t%s%2.1fth: %-10u (%lu samples)\n",
				bit == star ? "* " : "  ",
				plist[i], ovals[i], ocounts[i]);
		}
	}

	if (ovals)
		free(ovals);
	if (ocounts)
		free(ocounts);

	fprintf(stderr, "\t  min=%u, max=%u\n", s->min, s->max);
}

static char *escape_string(char *str)
{
	char *newstr = malloc(strlen(str) * 2 + 1);
	char *ptr = newstr;
	int maxlen = strlen(str) * 2;
	int len = strlen(str);

	if (!newstr) {
		perror("malloc");
		exit(1);
	}
	memcpy(newstr, str, strlen(str));
	while ((ptr = strchr(ptr, '"'))) {
		if (len == maxlen - 1) {
			free(newstr);
			return NULL;
		}
		memmove(ptr + 1, ptr, len - (ptr - newstr));
		*ptr = '\\';
		ptr += 2;
		len++;
	}
	newstr[len] = '\0';
	return newstr;
}

static void chomp(char *buf)
{
	size_t max = strlen(buf);
	size_t index = max - 1;

	if (max == 0)
		return;

	while (index && isspace(buf[index])) index--;
	index++;
	buf[index] = '\0';
}

static void print_sched_ext_info(FILE *fp)
{
	char buf[1024];
	FILE *tmpfile;
	size_t nr_read;

	tmpfile = fopen("/sys/kernel/sched_ext/state", "r");
	if (!tmpfile)
		goto no_sched_ext;
	nr_read = fread(buf, 1, 1023, tmpfile);
	buf[nr_read] = '\0';
	fclose(tmpfile);
	if (!strcmp(buf, "disabled"))
		goto no_sched_ext;
	tmpfile = fopen("/sys/kernel/sched_ext/root/ops", "r");
	if (!tmpfile)
		goto no_sched_ext;
	nr_read = fread(buf, 1, 1023, tmpfile);
	buf[nr_read] = '\0';
	chomp(buf);
	fclose(tmpfile);
	if (nr_read == 0)
		goto no_sched_ext;
	fprintf(fp, "\"sched_ext\": \"%s\",", buf);
	return;
no_sched_ext:
	fprintf(fp, "\"sched_ext\": \"disabled\",");
}

static void write_json_header(FILE *fp, char **argv, int argc)
{
	struct addrinfo hints, *info;
	struct utsname u[1024];
	time_t seconds;

	uname(u);

	seconds = time(NULL);
	fprintf(fp, "{");
	fprintf(fp, "\"normal\": {");
	fprintf(fp, "\"version\": \"%s\",", u->release);

	if (jobname)
		fprintf(fp, "\"jobname\": \"%s\",", jobname);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	if (getaddrinfo(u->nodename, NULL, &hints, &info) == 0) {
		fprintf(fp, "\"hostname\": \"%s\",", info->ai_canonname);
		freeaddrinfo(info);
	} else {
		fprintf(fp, "\"hostname\": \"%s\",", u->nodename);
	}

	print_sched_ext_info(fp);

	fprintf(fp, "\"cmdline\": \"");
	for (int i = 0; i < argc; i++) {
		if (i)
			fprintf(fp, " ");
		if (strchr(argv[i], '"')) {
			char *newstr = escape_string(argv[i]);
			if (!newstr) {
				fprintf(stderr, "escape_string failed\n");
				exit(1);
			}
			fprintf(fp, "%s", newstr);
			free(newstr);
		} else {
			fprintf(fp, "%s", argv[i]);
		}
	}
	fprintf(fp, "\"},");
	fprintf(fp, "\"int\": {\"time\": %lu, ", seconds);
}

static void write_json_stats(FILE *fp, struct stats *s, char *label)
{
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned int len, i;

	len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
	if (len) {
		for (i = 0; i < len; i++) {
			if (i)
				fprintf(fp, ", ");
			fprintf(fp, "\"%s_pct%2.1f\": %u", label, plist[i], ovals[i]);
		}
		fprintf(fp, ", \"%s_min\": %u,", label, s->min);
		fprintf(fp, "\"%s_max\": %u", label, s->max);
	}

	if (ovals)
		free(ovals);
	if (ocounts)
		free(ocounts);
}

static void write_json_footer(FILE *fp)
{
	fprintf(fp, "}}");
	fflush(fp);
}

/* fold latency info from s into d */
void combine_stats(struct stats *d, struct stats *s)
{
	int i;
	for (i = 0; i < PLAT_NR; i++)
		d->plat[i] += s->plat[i];
	d->nr_samples += s->nr_samples;
	if (s->max > d->max)
		d->max = s->max;
	if (d->min == 0 || s->min < d->min)
		d->min = s->min;
}

/* record a latency result into the histogram */
static void add_lat(struct stats *s, unsigned int us)
{
	int lat_index = 0;

	if (us > s->max)
		s->max = us;
	if (s->min == 0 || us < s->min)
		s->min = us;

	lat_index = plat_val_to_idx(us);
	__sync_fetch_and_add(&s->plat[lat_index], 1);
	__sync_fetch_and_add(&s->nr_samples, 1);
}

struct request {
	struct timeval start_time;
	struct request *next;
};

/*
 * every thread has one of these, it comes out to about 19K thanks to the
 * giant stats struct
 */
struct thread_data {
	pthread_t tid;
	/* used for pinning to CPUs etc, just a counter for which thread we are */
	unsigned long index;
	/* ->next is for placing us on the msg_thread's list for waking */
	struct thread_data *next;

	/* ->request is all of our pending request */
	struct request *request;

	/* our parent thread and messaging partner */
	struct thread_data *msg_thread;

	/*
	 * the msg thread stuffs gtod in here before waking us, so we can
	 * measure scheduler latency
	 */
	struct timeval wake_time;

	/* keep the futex and the wake_time in the same cacheline */
	int futex;

	/* mr axboe's magic latency histogram */
	struct stats wakeup_stats;
	struct stats request_stats;
	unsigned long long loop_count;
	unsigned long long runtime;
	unsigned long pending;

	char pipe_page[PIPE_TRANSFER_BUFFER];

	/* matrices to multiply */
	unsigned long *data;
};

/* we're so fancy we make our own futex wrappers */
#define FUTEX_BLOCKED 0
#define FUTEX_RUNNING 1

static int futex(int *uaddr, int futex_op, int val,
		 const struct timespec *timeout, int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

/*
 * wakeup a process waiting on a futex, making sure they are really waiting
 * first
 */
static void fpost(int *futexp)
{
	int s;

	if (__sync_bool_compare_and_swap(futexp, FUTEX_BLOCKED,
					 FUTEX_RUNNING)) {
		s = futex(futexp, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
		if (s  == -1) {
			perror("FUTEX_WAKE");
			exit(1);
		}
	}
}

/*
 * wait on a futex, with an optional timeout.  Make sure to set
 * the futex to FUTEX_BLOCKED beforehand.
 *
 * This will return zero if all went well, or return -ETIMEDOUT if you
 * hit the timeout without getting posted
 */
static int fwait(int *futexp, struct timespec *timeout)
{
	int s;
	while (1) {
		/* Is the futex available? */
		if (__sync_bool_compare_and_swap(futexp, FUTEX_RUNNING,
						 FUTEX_BLOCKED)) {
			break;      /* Yes */
		}
		/* Futex is not available; wait */
		s = futex(futexp, FUTEX_WAIT_PRIVATE, FUTEX_BLOCKED, timeout, NULL, 0);
		if (s == -1 && errno != EAGAIN) {
			if (errno == ETIMEDOUT)
				return -ETIMEDOUT;
			perror("futex-FUTEX_WAIT");
			exit(1);
		}
	}
	return 0;
}

/*
 * cmpxchg based list prepend
 */
static void xlist_add(struct thread_data *head, struct thread_data *add)
{
	struct thread_data *old;
	struct thread_data *ret;

	while (1) {
		old = head->next;
		add->next = old;
		ret = __sync_val_compare_and_swap(&head->next, old, add);
		if (ret == old)
			break;
	}
}

/*
 * xchg based list splicing.  This returns the entire list and
 * replaces the head->next with NULL
 */
static struct thread_data *xlist_splice(struct thread_data *head)
{
	struct thread_data *old;
	struct thread_data *ret;

	while (1) {
		old = head->next;
		ret = __sync_val_compare_and_swap(&head->next, old, NULL);
		if (ret == old)
			break;
	}
	return ret;
}

/*
 * cmpxchg based list prepend
 */
static struct request *request_add(struct thread_data *head, struct request *add)
{
	struct request *old;
	struct request *ret;

	while (1) {
		old = head->request;
		add->next = old;
		ret = __sync_val_compare_and_swap(&head->request, old, add);
		if (ret == old)
			return old;
	}
}

/*
 * xchg based list splicing.  This returns the entire list and
 * replaces the head->request with NULL.  The list is reversed before
 * returning
 */
static struct request *request_splice(struct thread_data *head)
{
	struct request *old;
	struct request *ret;
	struct request *reverse = NULL;

	while (1) {
		old = head->request;
		ret = __sync_val_compare_and_swap(&head->request, old, NULL);
		if (ret == old)
			break;
	}

	while(ret) {
		struct request *tmp = ret;
		ret = ret->next;
		tmp->next = reverse;
		reverse = tmp;
	}
	return reverse;
}

static struct request *allocate_request(void)
{
	struct request *ret = malloc(sizeof(*ret));

	if (!ret) {
		perror("malloc");
		exit(1);
	}

	gettimeofday(&ret->start_time, NULL);
	ret->next = NULL;
	return ret;
}


/*
 * Wake everyone currently waiting on the message list, filling in their
 * thread_data->wake_time with the current time.
 *
 * It's not exactly the current time, it's really the time at the start of
 * the list run.  We want to detect when the scheduler is just preempting the
 * waker and giving away the rest of its timeslice.  So we gtod once at
 * the start of the loop and use that for all the threads we wake.
 *
 * Since pipe mode ends up measuring this other ways, we do the gtod
 * every time in pipe mode
 */
static void xlist_wake_all(struct thread_data *td)
{
	struct thread_data *list;
	struct thread_data *next;
	struct timeval now;

	list = xlist_splice(td);
	gettimeofday(&now, NULL);
	while (list) {
		next = list->next;
		list->next = NULL;
		if (pipe_test) {
			memset(list->pipe_page, 1, pipe_test);
			gettimeofday(&list->wake_time, NULL);
		} else {
			memcpy(&list->wake_time, &now, sizeof(now));
		}
		fpost(&list->futex);
		list = next;
	}
}

/*
 * called by worker threads to send a message and wait for the answer.
 * In reality we're just trading one cacheline with the gtod and futex in
 * it, but that's good enough.  We gtod after waking and use that to
 * record scheduler latency.
 */
static struct request *msg_and_wait(struct thread_data *td)
{
	struct request *req;
	struct timeval now;
	unsigned long long delta;

	if (pipe_test)
		memset(td->pipe_page, 2, pipe_test);

	/* set ourselves to blocked */
	td->futex = FUTEX_BLOCKED;
	gettimeofday(&td->wake_time, NULL);

	/* add us to the list */
	if (requests_per_sec) {
		td->pending = 0;
		req = request_splice(td);
		if (req) {
			td->futex = FUTEX_RUNNING;
			return req;
		}
	} else {
		xlist_add(td->msg_thread, td);
	}

	fpost(&td->msg_thread->futex);

	/*
	 * don't wait if the main threads are shutting down,
	 * they will never kick us fpost has a full barrier, so as long
	 * as the message thread walks his list after setting stopping,
	 * we shouldn't miss the wakeup
	 */
	if (!stopping) {
		/* if he hasn't already woken us up, wait */
		fwait(&td->futex, NULL);
	}
	gettimeofday(&now, NULL);
	delta = tvdelta(&td->wake_time, &now);
	if (delta > 0)
		add_lat(&td->wakeup_stats, delta);

	return NULL;
}

/*
 * read /proc/stat, return the percentage of non-idle time since
 * the last read.
 */
float read_busy(int fd, char *buf, int len,
		unsigned long long *total_time_ret,
		unsigned long long *idle_time_ret)
{
	unsigned long long total_time = 0;
	unsigned long long idle_time = 0;
	unsigned long long delta;
	unsigned long long delta_idle;
	unsigned long long val;
	int col = 1;
	int ret;
	char *c;
	char *save = NULL;


	ret = lseek(fd, 0, SEEK_SET);
	if (ret < 0) {
		perror("lseek");
		exit(1);
	}
	ret = read(fd, buf, len-1);
	if (ret < 0) {
		perror("failed to read /proc/stat");
		exit(1);
	}
	buf[ret] = '\0';

	/* find the newline */
	c = strchr(buf, '\n');
	if (c == NULL) {
		perror("unable to parse /proc/stat");
		exit(1);
	}
	*c = '\0';

	/* cpu  590315893 45841886 375984879 82585100131 166708940 0 5453892 0 0 0 */
	c = strtok_r(buf, " ", &save);
	if (strcmp(c, "cpu") != 0) {
		perror("unable to parse summary in /proc/stat");
		exit(1);
	}

	while (c != NULL) {
		c = strtok_r(NULL, " ", &save);
		if (!c)
			break;
		val = atoll(c);
		if (col++ == 4)
			idle_time = val;
		total_time += val;
	}

	if (*total_time_ret == 0) {
		*total_time_ret = total_time;
		*idle_time_ret = idle_time;
		return 0.0;
	}

	/* delta is the total time spent doing everything */
	delta = total_time - *total_time_ret;
	delta_idle = idle_time - *idle_time_ret;

	*total_time_ret = total_time;
	*idle_time_ret = idle_time;

	return 100.00 - ((float)delta_idle/(float)delta) * 100.00;
}

#if defined(__x86_64__) || defined(__i386__)
#define nop __asm__ __volatile__("rep;nop": : :"memory")
#elif defined(__aarch64__)
#define nop __asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc64__)
#define nop __asm__ __volatile__("nop": : :"memory")
#else
#error Unsupported architecture
#endif

/*
 * once the message thread starts all his children, this is where he
 * loops until our runtime is up.  Basically this sits around waiting
 * for posting by the worker threads, and replying to their messages.
 */
static void run_msg_thread(struct thread_data *td)
{
	while (1) {
		td->futex = FUTEX_BLOCKED;
		xlist_wake_all(td);

		if (stopping) {
			xlist_wake_all(td);
			break;
		}
		fwait(&td->futex, NULL);
	}
}

void auto_scale_rps(int *proc_stat_fd,
		    unsigned long long *total_time,
		    unsigned long long *total_idle)
{
	int fd = *proc_stat_fd;
	float busy = 0;
	char proc_stat_buf[512];
	int first_run = 0;
	float delta;
	float target = 1;

	if (fd == -1) {
		fd = open("/proc/stat", O_RDONLY);
		if (fd < 0) {
			perror("unable to open /proc/stat");
			exit(1);
		}
		*proc_stat_fd = fd;
		first_run = 1;
	}
	busy = read_busy(fd, proc_stat_buf, 512, total_time, total_idle);
	if (first_run)
		return;
	if (busy < auto_rps) {
		delta = (float)auto_rps / busy;
		/* delta is > 1 */
		if (delta > 3) {
			delta = 3;
		} else if (delta < 1.2) {
			delta = 1 + (delta - 1) / 8;
			if (delta < 1.05 && !auto_rps_target_hit) {
				auto_rps_target_hit = 1;
				memset(&rps_stats, 0, sizeof(rps_stats));
			}

		} else if (delta < 1.5) {
			delta = 1 + (delta - 1) / 4;
		}
		target = ceilf((float)requests_per_sec * delta);
		if (target >= (1ULL << 31)) {
			/*
			 * sometimes we don't have enough threads to hit the
			 * target load
			 */
			target = requests_per_sec;
		}
	} else if (busy > auto_rps) {
		/* delta < 1 */
		delta = (float)auto_rps / busy;
		if (delta < 0.3) {
			delta = 0.3;
		} else if (delta > .9) {
			delta += (1 - delta) / 8;
			if (delta > .95 && !auto_rps_target_hit) {
				auto_rps_target_hit = 1;
				memset(&rps_stats, 0, sizeof(rps_stats));
			}
		} else if (delta > .8) {
			delta += (1 - delta) / 4;
		}
		target = floorf((float)requests_per_sec * delta);
		if (target <= 0)
			target = 0;
	} else {
		target = requests_per_sec;
		if (!auto_rps_target_hit) {
			auto_rps_target_hit = 1;
			memset(&rps_stats, 0, sizeof(rps_stats));
		}
	}
	requests_per_sec = target;
}

/*
 * once the message thread starts all his children, this is where he
 * loops until our runtime is up.  Basically this sits around waiting
 * for posting by the worker threads, replying to their messages.
 */
static void run_rps_thread(struct thread_data *worker_threads_mem)
{
	/* start and end of the thread run */
	struct timeval start;
	struct timeval now;
	struct request *request;
	unsigned long long delta;

	/* how long do we sleep between each wake */
	unsigned long batch = 128;
	int cur_tid = 0;
	int i;

	while (1) {
		gettimeofday(&start, NULL);
		for (i = 1; i < requests_per_sec + 1; i++) {
			struct thread_data *worker;

			if (stopping)
				break;
			gettimeofday(&now, NULL);

			worker = worker_threads_mem + cur_tid % worker_threads;
			cur_tid++;

			/* at some point, there's just too much, don't queue more */
			if (worker->pending > batch) {
				__sync_synchronize();
				if (worker->pending > batch) {
					usleep(100);
					continue;
				}
			}
			worker->pending++;
			request = allocate_request();
			request_add(worker, request);
			memcpy(&worker->wake_time, &now, sizeof(now));
			fpost(&worker->futex);
		}
		gettimeofday(&now, NULL);

		delta = tvdelta(&start, &now);
		while (!stopping && delta < USEC_PER_SEC) {
			delta = USEC_PER_SEC - delta;
			usleep(delta);

			gettimeofday(&now, NULL);
			delta = tvdelta(&start, &now);
		}

		if (stopping) {
			for (i = 0; i < worker_threads; i++)
				fpost(&worker_threads_mem[i].futex);
			break;
		}
	}
}

/*
 * multiply two matrices in a naive way to emulate some cache footprint
 */
static void do_some_math(struct thread_data *thread_data)
{
	unsigned long i, j, k;
	unsigned long *m1, *m2, *m3;

	m1 = &thread_data->data[0];
	m2 = &thread_data->data[matrix_size * matrix_size];
	m3 = &thread_data->data[2 * matrix_size * matrix_size];

	for (i = 0; i < matrix_size; i++) {
		for (j = 0; j < matrix_size; j++) {
			m3[i * matrix_size + j] = 0;

			for (k = 0; k < matrix_size; k++)
				m3[i * matrix_size + j] +=
					m1[i * matrix_size + k] *
					m2[k * matrix_size + j];
		}
	}
}

static pthread_mutex_t *lock_this_cpu(void)
{
	int cpu;
	int cur_cpu;
	pthread_mutex_t *lock;

again:
	cpu = sched_getcpu();
	if (cpu < 0) {
		perror("sched_getcpu failed\n");
		exit(1);
	}
	lock = &per_cpu_locks[cpu].lock;
	while (pthread_mutex_trylock(lock) != 0)
		nop;

	cur_cpu = sched_getcpu();
	if (cur_cpu < 0) {
		perror("sched_getcpu failed\n");
		exit(1);
	}

	if (cur_cpu != cpu) {
		/* we got the lock but we migrated */
		pthread_mutex_unlock(lock);
		goto again;
	}
	return lock;

}

/*
 * spin or do some matrix arithmetic
 */
static void do_work(struct thread_data *td)
{
	pthread_mutex_t *lock = NULL;
	unsigned long i;

	/* using --calibrate or --no-locking skips the locks */
	if (!skip_locking)
		lock = lock_this_cpu();
	for (i = 0; i < operations; i++)
		do_some_math(td);
	if (!skip_locking)
		pthread_mutex_unlock(lock);
}

/*
 * the worker thread is pretty simple, it just does a single spin and
 * then waits on a message from the message thread
 */
void *worker_thread(void *arg)
{
	struct thread_data *td = arg;
	struct timeval now;
	struct timeval work_start;
	struct timeval start;
	unsigned long long delta;
	struct request *req = NULL;

	gettimeofday(&start, NULL);
	while(1) {
		if (stopping)
			break;

		req = msg_and_wait(td);
		if (requests_per_sec && !req)
			continue;

		do {
			struct request *tmp;

			if (pipe_test) {
				gettimeofday(&work_start, NULL);
			} else {
				if (calibrate_only) {
					/*
					 * in calibration mode, don't include the
					 * usleep in the timing
					 */
					if (sleep_usec > 0)
						usleep(sleep_usec);
					gettimeofday(&work_start, NULL);
				} else {
					/*
					 * lets start off with some simulated networking,
					 * and also make sure we get a fresh clean timeslice
					 */
					gettimeofday(&work_start, NULL);
					if (sleep_usec > 0)
						usleep(sleep_usec);
				}
				do_work(td);
			}

			gettimeofday(&now, NULL);

			td->runtime = tvdelta(&start, &now);
			if (req) {
				tmp = req->next;
				free(req);
				req = tmp;
			}
			td->loop_count++;

			delta = tvdelta(&work_start, &now);
			if (delta > 0)
				add_lat(&td->request_stats, delta);
		} while (req);
	}
	gettimeofday(&now, NULL);
	td->runtime = tvdelta(&start, &now);

	return NULL;
}

int find_nth_set_bit(const cpu_set_t *set, int n)
{
	int count = 0;
	for (int i = 0; i < CPU_SETSIZE; ++i) {
		if (CPU_ISSET(i, set)) {
			if (count == n)
				return i; // Return the CPU index of the n’th set bit
			++count;
		}
	}
	return -1; // Not found
}

static void pin_message_cpu(int index, cpu_set_t *possible_cpus)
{
	cpu_set_t cpuset;
	int ret;
	CPU_ZERO(&cpuset);
	int num_possible = CPU_COUNT(possible_cpus);
	int cpu_to_set = index % num_possible;

	cpu_to_set = find_nth_set_bit(possible_cpus, cpu_to_set);
	CPU_SET(cpu_to_set, &cpuset); // Pin to CPU 0

	pthread_t thread = pthread_self();
	ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (ret) {
		fprintf(stderr, "unable to set CPU affinity to cpu %d\n", cpu_to_set);
		exit(1);
	}
	fprintf(stderr, "Pinning to message thread index %d cpu %d\n", index, cpu_to_set);
}

static void pin_worker_cpus(cpu_set_t *worker_cpus)
{
	int ret;
	pthread_t thread = pthread_self();
	ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), worker_cpus);
	if (ret) {
		fprintf(stderr, "unable to set CPU affinity\n");
	}
}

/*
 * the message thread starts his own gaggle of workers and then sits around
 * replying when they post him.  He collects latency stats as all the threads
 * exit
 */
void *message_thread(void *arg)
{
	struct thread_data *td = arg;
	struct thread_data *worker_threads_mem = NULL;
	int i;
	int ret;

	worker_threads_mem = td + 1;

	if (!worker_threads_mem) {
		perror("unable to allocate ram");
		pthread_exit((void *)-ENOMEM);
	}

	if (worker_cpus)
		pin_worker_cpus(worker_cpus);

	for (i = 0; i < worker_threads; i++) {
		pthread_t tid;
		worker_threads_mem[i].data = malloc(3 * sizeof(unsigned long) * matrix_size * matrix_size);
		if (!worker_threads_mem[i].data) {
			perror("unable to allocate ram");
			pthread_exit((void *)-ENOMEM);
		}

		worker_threads_mem[i].msg_thread = td;
		ret = pthread_create(&tid, NULL, worker_thread,
				     worker_threads_mem + i);
		if (ret) {
			fprintf(stderr, "error %d from pthread_create\n", ret);
			exit(1);
		}
		worker_threads_mem[i].tid = tid;
		worker_threads_mem[i].index = i;
	}

	if (message_cpus)
		pin_message_cpu(td->index, message_cpus);

	if (requests_per_sec)
		run_rps_thread(worker_threads_mem);
	else
		run_msg_thread(td);

	for (i = 0; i < worker_threads; i++) {
		fpost(&worker_threads_mem[i].futex);
		pthread_join(worker_threads_mem[i].tid, NULL);
	}
	return NULL;
}

static char *units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB", NULL};

static double pretty_size(double number, char **str)
{
	int divs = 0;

	while(number >= 1024) {
		if (units[divs + 1] == NULL)
			break;
		divs++;
		number /= 1024;
	}
	*str = units[divs];
	return number;
}

/*
 * we want to calculate RPS more often than the full message stats,
 * so this is a less expensive walk through all the message threads
 * to pull that out
 */
static void combine_message_thread_rps(struct thread_data *thread_data,
				       unsigned long long *loop_count)
{
	struct thread_data *worker;
	int i;
	int msg_i;
	int index = 0;

	*loop_count = 0;
	for (msg_i = 0; msg_i < message_threads; msg_i++) {
		index++;
		for (i = 0; i < worker_threads; i++) {
			worker = thread_data + index++;
			*loop_count += worker->loop_count;
		}
	}
}

static void combine_message_thread_stats(struct stats *wakeup_stats,
					 struct stats *request_stats,
					struct thread_data *thread_data,
					unsigned long long *loop_count,
					unsigned long long *loop_runtime)
{
	struct thread_data *worker;
	int i;
	int msg_i;
	int index = 0;

	*loop_count = 0;
	*loop_runtime = 0;
	for (msg_i = 0; msg_i < message_threads; msg_i++) {
		index++;
		for (i = 0; i < worker_threads; i++) {
			worker = thread_data + index++;
			combine_stats(wakeup_stats, &worker->wakeup_stats);
			combine_stats(request_stats, &worker->request_stats);
			*loop_count += worker->loop_count;
			*loop_runtime += worker->runtime;
		}
	}
}

static void reset_thread_stats(struct thread_data *thread_data)
{
	struct thread_data *worker;
	int i;
	int msg_i;
	int index = 0;

	memset(&rps_stats, 0, sizeof(rps_stats));
	for (msg_i = 0; msg_i < message_threads; msg_i++) {
		index++;
		for (i = 0; i < worker_threads; i++) {
			worker = thread_data + index++;
			memset(&worker->wakeup_stats, 0, sizeof(worker->wakeup_stats));
			memset(&worker->request_stats, 0, sizeof(worker->request_stats));
		}
	}
}

/* runtime from the command line is in seconds.  Sleep until its up */
static void sleep_for_runtime(struct thread_data *message_threads_mem)
{
	struct timeval now;
	struct timeval zero_time;
	struct timeval last_calc;
	struct timeval last_rps_calc;
	struct timeval start;
	struct stats wakeup_stats;
	struct stats request_stats;
	unsigned long long last_loop_count = 0;
	unsigned long long loop_count;
	unsigned long long loop_runtime;
	unsigned long long delta;
	unsigned long long runtime_delta;
	unsigned long long runtime_usec = runtime * USEC_PER_SEC;
	unsigned long long warmup_usec = warmuptime * USEC_PER_SEC;
	unsigned long long interval_usec = intervaltime * USEC_PER_SEC;
	unsigned long long zero_usec = zerotime * USEC_PER_SEC;
	int warmup_done = 0;
	int total_intervals = 0;

	/* if we're autoscaling RPS */
	int proc_stat_fd = -1;
	unsigned long long total_time = 0;
	unsigned long long total_idle = 0;
	int done = 0;

	memset(&wakeup_stats, 0, sizeof(wakeup_stats));
	gettimeofday(&start, NULL);
	last_calc = start;
	last_rps_calc = start;
	zero_time = start;

	while(!done) {
		gettimeofday(&now, NULL);
		runtime_delta = tvdelta(&start, &now);

		if (runtime_usec && runtime_delta >= runtime_usec)
			done = 1;

		if (!requests_per_sec && !pipe_test &&
		    runtime_delta > warmup_usec &&
		    !warmup_done && warmuptime) {
			warmup_done = 1;
			fprintf(stderr, "warmup done, zeroing stats\n");
			zero_time = now;
			reset_thread_stats(message_threads_mem);
		} else if (!pipe_test) {
			double rps;

			/* count our RPS every round */
			delta = tvdelta(&last_rps_calc, &now);

			combine_message_thread_rps(message_threads_mem, &loop_count);
			rps = (double)((loop_count - last_loop_count) * USEC_PER_SEC) / delta;
			last_loop_count = loop_count;
			last_rps_calc = now;

			if (!auto_rps || auto_rps_target_hit)
				add_lat(&rps_stats, isfinite(rps) ? rps : 0);

			delta = tvdelta(&last_calc, &now);
			if (delta >= interval_usec) {

				memset(&wakeup_stats, 0, sizeof(wakeup_stats));
				memset(&request_stats, 0, sizeof(request_stats));
				combine_message_thread_stats(&wakeup_stats,
					     &request_stats, message_threads_mem,
					     &loop_count, &loop_runtime);
				last_calc = now;

				show_latencies(&wakeup_stats, "Wakeup Latencies",
					       "usec", runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_LAT, PLIST_99);
				show_latencies(&request_stats, "Request Latencies",
					       "usec", runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_LAT, PLIST_99);
				show_latencies(&rps_stats, "RPS",
					       "requests", runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_RPS, PLIST_50);
				fprintf(stderr, "current rps: %.2f\n", rps);
				total_intervals++;
			}
		}
		if (zero_usec) {
			unsigned long long zero_delta;
			zero_delta = tvdelta(&zero_time, &now);
			if (zero_delta > zero_usec) {
				zero_time = now;
				reset_thread_stats(message_threads_mem);
			}
		}
		if (auto_rps)
			auto_scale_rps(&proc_stat_fd, &total_time, &total_idle);
		if (!done)
			sleep(1);
	}
	if (proc_stat_fd >= 0)
		close(proc_stat_fd);
	__sync_synchronize();
	stopping = 1;
}


int main(int ac, char **av)
{
	int i;
	int ret;
	struct thread_data *message_threads_mem = NULL;
	struct stats wakeup_stats;
	struct stats request_stats;
	double loops_per_sec;
	unsigned long long loop_count;
	unsigned long long loop_runtime;

	parse_options(ac, av);

	if (worker_threads == 0) {
		unsigned long num_cpus = get_nprocs();

		worker_threads = (num_cpus + message_threads - 1) / message_threads;

		fprintf(stderr, "setting worker threads to %d\n", worker_threads);
	}

	matrix_size = sqrt(cache_footprint_kb * 1024 / 3 / sizeof(unsigned long));

	num_cpu_locks = get_nprocs();
	per_cpu_locks = calloc(num_cpu_locks, sizeof(struct per_cpu_lock));
	if (!per_cpu_locks) {
		perror("unable to allocate memory for per cpu locks\n");
		exit(1);
	}

	for (i = 0; i < num_cpu_locks; i++) {
		pthread_mutex_t *lock = &per_cpu_locks[i].lock;
		ret = pthread_mutex_init(lock, NULL);
		if (ret) {
			perror("mutex init failed\n");
			exit(1);
		}
	}

	requests_per_sec /= message_threads;
	loops_per_sec = 0;
	stopping = 0;
	memset(&wakeup_stats, 0, sizeof(wakeup_stats));
	memset(&request_stats, 0, sizeof(request_stats));
	memset(&rps_stats, 0, sizeof(rps_stats));

	message_threads_mem = calloc(message_threads * worker_threads + message_threads,
				      sizeof(struct thread_data));


	if (!message_threads_mem) {
		perror("unable to allocate message threads");
		exit(1);
	}

	/* start our message threads, each one starts its own workers */
	for (i = 0; i < message_threads; i++) {
		pthread_t tid;
		int index = i * worker_threads + i;
		struct thread_data *td = message_threads_mem + index;
		td->index = i;
		ret = pthread_create(&tid, NULL, message_thread,
				     message_threads_mem + index);
		if (ret) {
			fprintf(stderr, "error %d from pthread_create\n", ret);
			exit(1);
		}
		message_threads_mem[index].tid = tid;
	}

	sleep_for_runtime(message_threads_mem);

	for (i = 0; i < message_threads; i++) {
		int index = i * worker_threads + i;
		fpost(&message_threads_mem[index].futex);
		pthread_join(message_threads_mem[index].tid, NULL);
	}
	memset(&wakeup_stats, 0, sizeof(wakeup_stats));
	memset(&request_stats, 0, sizeof(request_stats));
	combine_message_thread_stats(&wakeup_stats, &request_stats,
				     message_threads_mem,
				     &loop_count, &loop_runtime);

	loops_per_sec = loop_count * USEC_PER_SEC;
	loops_per_sec /= loop_runtime;

	free(message_threads_mem);

	if (json_file) {
		FILE *outfile;

		if (strcmp(json_file, "-") == 0)
			outfile = stdout;
		else
			outfile = fopen(json_file, "w");
		if (!json_file) {
			perror("unable to open json file");
			exit(1);
		}
		write_json_header(outfile, av, ac);
		write_json_stats(outfile, &wakeup_stats, "wakeup_latency");
		if (!pipe_test) {
			fprintf(outfile, ", ");
			write_json_stats(outfile, &request_stats,
					 "request_latency");
			fprintf(outfile, ", ");
			write_json_stats(outfile, &rps_stats,
					 "rps");
		}
		fprintf(outfile, ", \"runtime\": %u", runtime);
		write_json_footer(outfile);
		if (outfile != stdout)
			fclose(outfile);
	}

	if (pipe_test) {
		char *pretty;
		double mb_per_sec;

		show_latencies(&wakeup_stats, "Wakeup Latencies", "usec", runtime,
			       PLIST_20 | PLIST_FOR_LAT, PLIST_99);

		mb_per_sec = (loop_count * pipe_test * USEC_PER_SEC) / loop_runtime;
		mb_per_sec = pretty_size(mb_per_sec, &pretty);
		fprintf(stderr, "avg worker transfer: %.2f ops/sec %.2f%s/s\n",
		       loops_per_sec, mb_per_sec, pretty);
	} else {
		show_latencies(&wakeup_stats, "Wakeup Latencies", "usec", runtime,
			       PLIST_FOR_LAT, PLIST_99);
		show_latencies(&request_stats, "Request Latencies", "usec", runtime,
			       PLIST_FOR_LAT, PLIST_99);
		show_latencies(&rps_stats, "RPS", "requests", runtime,
			       PLIST_FOR_RPS, PLIST_50);
		if (!auto_rps) {
			fprintf(stderr, "average rps: %.2f\n",
				(double)(loop_count) / runtime);
		} else {
			fprintf(stderr, "final rps goal was %d\n",
	                        requests_per_sec * message_threads);
		}
	}

	return 0;
}
