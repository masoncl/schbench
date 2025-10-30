/*
 * schbench.c - Main program
 *
 * Copyright (C) 2016 Facebook
 * Chris Mason <clm@fb.com>
 *
 * GPLv2
 */
#include <string.h>
#include <math.h>
#include <sys/sysinfo.h>
#include "schbench.h"
#include "options.h"
#include "stats.h"
#include "futex.h"
#include "utils.h"
#include "message.h"
#include "topology.h"

/* Global variables */
volatile unsigned long stopping = 0;
unsigned long matrix_size = 0;
struct per_cpu_lock *per_cpu_locks;
int num_cpu_locks;
struct stats rps_stats;

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
	unsigned long long message_thread_delay;
	unsigned long long worker_thread_delay;
	int warmup_done = 0;

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

	while (!done) {
		gettimeofday(&now, NULL);
		runtime_delta = tvdelta(&start, &now);

		if (runtime_usec && runtime_delta >= runtime_usec)
			done = 1;

		if (!requests_per_sec && !pipe_test &&
		    runtime_delta > warmup_usec && !warmup_done && warmuptime) {
			warmup_done = 1;
			fprintf(stderr, "warmup done, zeroing stats\n");
			zero_time = now;
			reset_thread_stats(message_threads_mem);
		} else if (!pipe_test) {
			double rps;

			/* count our RPS every round */
			delta = tvdelta(&last_rps_calc, &now);

			combine_message_thread_rps(message_threads_mem,
						   &loop_count);
			rps = (double)((loop_count - last_loop_count) *
				       USEC_PER_SEC) /
			      delta;
			last_loop_count = loop_count;
			last_rps_calc = now;

			if (!auto_rps || auto_rps_target_hit)
				add_lat(&rps_stats, isfinite(rps) ? rps : 0);

			delta = tvdelta(&last_calc, &now);
			if (delta >= interval_usec) {
				memset(&wakeup_stats, 0, sizeof(wakeup_stats));
				memset(&request_stats, 0,
				       sizeof(request_stats));
				combine_message_thread_stats(
					&wakeup_stats, &request_stats,
					message_threads_mem, &loop_count,
					&loop_runtime);
				collect_sched_delay(message_threads_mem,
						    &message_thread_delay,
						    &worker_thread_delay);
				last_calc = now;

				show_latencies(&wakeup_stats,
					       "Wakeup Latencies", "usec",
					       runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_LAT, PLIST_99);
				show_latencies(&request_stats,
					       "Request Latencies", "usec",
					       runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_LAT, PLIST_99);
				show_latencies(&rps_stats, "RPS", "requests",
					       runtime_delta / USEC_PER_SEC,
					       PLIST_FOR_RPS, PLIST_50);
				fprintf(stderr,
					"sched delay: message %llu (usec) worker %llu (usec)\n",
					message_thread_delay / 1000,
					worker_thread_delay / 1000);
				fprintf(stderr, "current rps: %.2f\n", rps);
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

		worker_threads =
			(num_cpus + message_threads - 1) / message_threads;

		fprintf(stderr, "setting worker threads to %d\n",
			worker_threads);
	}

	matrix_size =
		sqrt(cache_footprint_kb * 1024 / 3 / sizeof(unsigned long));

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

	message_threads_mem =
		calloc(message_threads * worker_threads + message_threads,
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

		if (pin_mode == PIN_MODE_CCX) {
			td->cpus = &per_message_thread_cpus[i];
		} else {
			td->cpus = message_cpus;
		}
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
				     message_threads_mem, &loop_count,
				     &loop_runtime);

	loops_per_sec = loop_count * USEC_PER_SEC;
	loops_per_sec /= loop_runtime;

	if (json_file) {
		FILE *outfile;

		if (strcmp(json_file, "-") == 0)
			outfile = stdout;
		else
			outfile = fopen(json_file, "w");
		if (!outfile) {
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
			write_json_stats(outfile, &rps_stats, "rps");
		}
		fprintf(outfile, ", \"runtime\": %u", runtime);
		write_json_footer(outfile);
		if (outfile != stdout)
			fclose(outfile);
	}

	if (pipe_test) {
		char *pretty;
		double mb_per_sec;

		show_latencies(&wakeup_stats, "Wakeup Latencies", "usec",
			       runtime, PLIST_20 | PLIST_FOR_LAT, PLIST_99);

		mb_per_sec =
			(loop_count * pipe_test * USEC_PER_SEC) / loop_runtime;
		mb_per_sec = pretty_size(mb_per_sec, &pretty);
		fprintf(stderr, "avg worker transfer: %.2f ops/sec %.2f%s/s\n",
			loops_per_sec, mb_per_sec, pretty);
	} else {
		unsigned long long message_thread_delay, worker_thread_delay;
		show_latencies(&wakeup_stats, "Wakeup Latencies", "usec",
			       runtime, PLIST_FOR_LAT, PLIST_99);
		show_latencies(&request_stats, "Request Latencies", "usec",
			       runtime, PLIST_FOR_LAT, PLIST_99);
		show_latencies(&rps_stats, "RPS", "requests", runtime,
			       PLIST_FOR_RPS, PLIST_50);
		if (!auto_rps) {
			fprintf(stderr, "average rps: %.2f\n",
				(double)(loop_count) / runtime);
		} else {
			fprintf(stderr, "final rps goal was %d\n",
				requests_per_sec * message_threads);
		}
		collect_sched_delay(message_threads_mem, &message_thread_delay,
				    &worker_thread_delay);

		fprintf(stderr,
			"sched delay: message %llu (usec) worker %llu (usec)\n",
			message_thread_delay / 1000,
			worker_thread_delay / 1000);
	}
	free(message_threads_mem);

	/* Clean up topology structures */
	if (pin_mode == PIN_MODE_CCX) {
		free(per_message_thread_cpus);
		free_topology(&topology);
	}

	return 0;
}
