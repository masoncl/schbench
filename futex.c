/*
 * futex.c - Futex wrappers and synchronization implementation
 */
#include <sys/syscall.h>
#include <string.h>
#include "futex.h"

int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout,
	  int *uaddr2, int val3)
{
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

/*
 * wakeup a process waiting on a futex, making sure they are really waiting
 * first
 */
void fpost(int *futexp)
{
	int s;

	if (__sync_bool_compare_and_swap(futexp, FUTEX_BLOCKED,
					 FUTEX_RUNNING)) {
		s = futex(futexp, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
		if (s == -1) {
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
int fwait(int *futexp, struct timespec *timeout)
{
	int s;
	while (1) {
		/* Is the futex available? */
		if (__sync_bool_compare_and_swap(futexp, FUTEX_RUNNING,
						 FUTEX_BLOCKED)) {
			break; /* Yes */
		}
		/* Futex is not available; wait */
		s = futex(futexp, FUTEX_WAIT_PRIVATE, FUTEX_BLOCKED, timeout,
			  NULL, 0);
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
void xlist_add(struct thread_data *head, struct thread_data *add)
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
struct thread_data *xlist_splice(struct thread_data *head)
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
struct request *request_add(struct thread_data *head, struct request *add)
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
struct request *request_splice(struct thread_data *head)
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

	while (ret) {
		struct request *tmp = ret;
		ret = ret->next;
		tmp->next = reverse;
		reverse = tmp;
	}
	return reverse;
}

struct request *allocate_request(void)
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
void xlist_wake_all(struct thread_data *td)
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
