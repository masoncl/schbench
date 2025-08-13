/*
 * worker.c - Worker thread implementation
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include "worker.h"
#include "futex.h"
#include "utils.h"
#include "stats.h"

/*
 * called by worker threads to send a message and wait for the answer.
 * In reality we're just trading one cacheline with the gtod and futex in
 * it, but that's good enough.  We gtod after waking and use that to
 * record scheduler latency.
 */
struct request *msg_and_wait(struct thread_data *td)
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
	int ret;

	td->sys_tid = get_sys_tid();

	if (td->cpus)
		pin_worker_cpus(td->cpus);

	ret = pthread_setname_np(pthread_self(), "schbench-worker");
	if (ret) {
		perror("failed to set worker thread name");
		exit(1);
	}
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
