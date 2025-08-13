/*
 * schbench.h - Main header file for schbench
 *
 * Copyright (C) 2016 Facebook
 * Chris Mason <clm@fb.com>
 *
 * GPLv2
 */
#ifndef _SCHBENCH_H
#define _SCHBENCH_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/types.h>
#include <linux/futex.h>

#define PLAT_BITS	8
#define PLAT_VAL	(1 << PLAT_BITS)
#define PLAT_GROUP_NR	19
#define PLAT_NR		(PLAT_GROUP_NR * PLAT_VAL)
#define PLAT_LIST_MAX	20

/* when -p is on, how much do we send back and forth */
#define PIPE_TRANSFER_BUFFER (1 * 1024 * 1024)

#define USEC_PER_SEC (1000000)

/* we're so fancy we make our own futex wrappers */
#define FUTEX_BLOCKED 0
#define FUTEX_RUNNING 1

/* Forward declarations */
struct thread_data;
struct stats;
struct request;
struct cpu_topology;

/* Global configuration variables */
extern int message_threads;
extern int worker_threads;
extern int runtime;
extern int warmuptime;
extern int intervaltime;
extern int zerotime;
extern unsigned long cache_footprint_kb;
extern unsigned long operations;
extern unsigned long sleep_usec;
extern int auto_rps;
extern int auto_rps_target_hit;
extern int pipe_test;
extern int requests_per_sec;
extern int calibrate_only;
extern int skip_locking;
extern char *json_file;
extern char *jobname;
extern volatile unsigned long stopping;
extern unsigned long matrix_size;

/* Pinning modes */
enum pin_mode {
	PIN_MODE_NONE = 0,
	PIN_MODE_MANUAL,
	PIN_MODE_AUTO,
	PIN_MODE_CCX
};

extern int pin_mode;
extern cpu_set_t *message_cpus;
extern cpu_set_t *worker_cpus;
extern struct cpu_topology topology;
extern cpu_set_t *per_message_thread_cpus;

/* Per-CPU lock structure */
struct per_cpu_lock {
	pthread_mutex_t lock;
} __attribute__((aligned));

extern struct per_cpu_lock *per_cpu_locks;
extern int num_cpu_locks;

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

extern struct stats rps_stats;

/* Request structure for RPS mode */
struct request {
	struct timeval start_time;
	struct request *next;
};

/*
 * every thread has one of these, it comes out to about 19K thanks to the
 * giant stats struct
 */
struct thread_data {
	/* opaque pthread tid */
	pthread_t tid;

	/* actual tid from SYS_gettid */
	unsigned long sys_tid;

	/* used for pinning to CPUs etc, just a counter for which thread we are */
	unsigned long index;
	/* ->next is for placing us on the msg_thread's list for waking */
	struct thread_data *next;

	/* ->request is all of our pending request */
	struct request *request;

	/* our parent thread and messaging partner */
	struct thread_data *msg_thread;

	/* if we're pinning, the CPU set to use */
	cpu_set_t *cpus;

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
	unsigned long long avg_sched_delay;
	unsigned long long loop_count;
	unsigned long long runtime;
	unsigned long pending;

	char pipe_page[PIPE_TRANSFER_BUFFER];

	/* matrices to multiply */
	unsigned long *data;
};

/* Architecture-specific nop instruction */
#if defined(__x86_64__) || defined(__i386__)
#define nop __asm__ __volatile__("rep;nop": : :"memory")
#elif defined(__aarch64__)
#define nop __asm__ __volatile__("yield" ::: "memory")
#elif defined(__powerpc64__)
#define nop __asm__ __volatile__("nop": : :"memory")
#else
#error Unsupported architecture
#endif

#endif /* _SCHBENCH_H */
