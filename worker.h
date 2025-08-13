/*
 * worker.h - Worker thread functions
 */
#ifndef _WORKER_H
#define _WORKER_H

#include "schbench.h"

/* Worker thread main function */
void *worker_thread(void *arg);

/* Worker utilities */
struct request *msg_and_wait(struct thread_data *td);

#endif /* _WORKER_H */
