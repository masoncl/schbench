/*
 * message.h - Message thread functions
 */
#ifndef _MESSAGE_H
#define _MESSAGE_H

#include "schbench.h"

/* Message thread main function */
void *message_thread(void *arg);

/* Message thread utilities */
void run_msg_thread(struct thread_data *td);
void run_rps_thread(struct thread_data *worker_threads_mem);
void auto_scale_rps(int *proc_stat_fd,
                    unsigned long long *total_time,
                    unsigned long long *total_idle);

/* Statistics collection */
void combine_message_thread_rps(struct thread_data *thread_data,
                               unsigned long long *loop_count);
void collect_sched_delay(struct thread_data *thread_data,
                        unsigned long long *message_thread_delay_ret,
                        unsigned long long *worker_thread_delay_ret);
void combine_message_thread_stats(struct stats *wakeup_stats,
                                 struct stats *request_stats,
                                 struct thread_data *thread_data,
                                 unsigned long long *loop_count,
                                 unsigned long long *loop_runtime);
void reset_thread_stats(struct thread_data *thread_data);

#endif /* _MESSAGE_H */
