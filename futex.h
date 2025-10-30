/*
 * futex.h - Futex wrappers and synchronization primitives
 */
#ifndef _FUTEX_H
#define _FUTEX_H

#include "schbench.h"
#include <time.h>

/* Futex wrapper functions */
int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout,
	  int *uaddr2, int val3);
void fpost(int *futexp);
int fwait(int *futexp, struct timespec *timeout);

/* List operations for thread_data */
void xlist_add(struct thread_data *head, struct thread_data *add);
struct thread_data *xlist_splice(struct thread_data *head);
void xlist_wake_all(struct thread_data *td);

/* Request list operations */
struct request *request_add(struct thread_data *head, struct request *add);
struct request *request_splice(struct thread_data *head);
struct request *allocate_request(void);

#endif /* _FUTEX_H */
