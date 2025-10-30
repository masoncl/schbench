/*
 * message.c - Message thread implementation
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <math.h>
#include "message.h"
#include "worker.h"
#include "futex.h"
#include "utils.h"
#include "stats.h"

/*
 * once the message thread starts all his children, this is where he
 * loops until our runtime is up.  Basically this sits around waiting
 * for posting by the worker threads, and replying to their messages.
 */
void run_msg_thread(struct thread_data *td)
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

void auto_scale_rps(int *proc_stat_fd, unsigned long long *total_time,
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
void run_rps_thread(struct thread_data *worker_threads_mem)
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

	if (td->cpus)
		pin_message_cpu(td->index, td->cpus);

	ret = pthread_setname_np(pthread_self(), "schbench-msg");
	if (ret) {
		perror("failed to set message thread name");
		exit(1);
	}
	worker_threads_mem = td + 1;

	if (!worker_threads_mem) {
		perror("unable to allocate ram");
		pthread_exit((void *)-ENOMEM);
	}

	td->sys_tid = get_sys_tid();
	for (i = 0; i < worker_threads; i++) {
		pthread_t tid;
		worker_threads_mem[i].data = malloc(3 * sizeof(unsigned long) *
						    matrix_size * matrix_size);
		if (!worker_threads_mem[i].data) {
			perror("unable to allocate ram");
			pthread_exit((void *)-ENOMEM);
		}

		worker_threads_mem[i].msg_thread = td;
		worker_threads_mem[i].index = i;

		if (pin_mode == PIN_MODE_CCX)
			worker_threads_mem[i].cpus =
				&per_message_thread_cpus[td->index];
		else
			worker_threads_mem[i].cpus = worker_cpus;

		ret = pthread_create(&tid, NULL, worker_thread,
				     worker_threads_mem + i);
		if (ret) {
			fprintf(stderr, "error %d from pthread_create\n", ret);
			exit(1);
		}
		worker_threads_mem[i].tid = tid;
		worker_threads_mem[i].index = i;
	}

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

/*
 * we want to calculate RPS more often than the full message stats,
 * so this is a less expensive walk through all the message threads
 * to pull that out
 */
void combine_message_thread_rps(struct thread_data *thread_data,
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

/*
 * read /proc/pid/schedstat for each of our threads and average out the delay
 * recorded on the kernel side for scheduling us.  This should be similar
 * to the delays we record between wakeup and actually running, but differences
 * can expose problems in different parts of the wakeup paths
 */
void collect_sched_delay(struct thread_data *thread_data,
			 unsigned long long *message_thread_delay_ret,
			 unsigned long long *worker_thread_delay_ret)
{
	struct thread_data *worker;
	unsigned long long message_thread_delay = 0;
	unsigned long long worker_thread_delay = 0;
	unsigned long long delay;
	int i;
	int msg_i;
	int index = 0;

	for (msg_i = 0; msg_i < message_threads; msg_i++) {
		delay = read_sched_delay(thread_data[index].sys_tid);
		message_thread_delay += delay;
		index++;
		for (i = 0; i < worker_threads; i++) {
			worker = thread_data + index++;
			delay = read_sched_delay(worker->sys_tid);
			worker_thread_delay += delay;
		}
	}
	*message_thread_delay_ret = message_thread_delay / message_threads;
	*worker_thread_delay_ret =
		worker_thread_delay / (message_threads * worker_threads);
}

void combine_message_thread_stats(struct stats *wakeup_stats,
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

void reset_thread_stats(struct thread_data *thread_data)
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
			worker->avg_sched_delay = 0;
			memset(&worker->wakeup_stats, 0,
			       sizeof(worker->wakeup_stats));
			memset(&worker->request_stats, 0,
			       sizeof(worker->request_stats));
		}
	}
}
