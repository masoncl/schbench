/*
 * utils.h - Utility functions
 */
#ifndef _UTILS_H
#define _UTILS_H

#include "schbench.h"

/* Time functions */
void tvsub(struct timeval *tdiff, struct timeval *t1, struct timeval *t0);
unsigned long long tvdelta(struct timeval *start, struct timeval *stop);

/* System utilities */
unsigned long get_sys_tid(void);
unsigned long long read_sched_delay(pid_t tid);
float read_busy(int fd, char *buf, int len, unsigned long long *total_time_ret,
		unsigned long long *idle_time_ret);

/* CPU affinity */
int find_nth_set_bit(const cpu_set_t *set, int n);
void pin_worker_cpus(cpu_set_t *worker_cpus);
void pin_message_cpu(int index, cpu_set_t *possible_cpus);

/* Work functions */
void do_some_math(struct thread_data *thread_data);
void do_work(struct thread_data *td);
pthread_mutex_t *lock_this_cpu(void);

/* JSON output */
void write_json_header(FILE *fp, char **argv, int argc);
void write_json_footer(FILE *fp);
void print_sched_ext_info(FILE *fp);
char *escape_string(char *str);
void chomp(char *buf);

/* Pretty print */
double pretty_size(double number, char **str);

#endif /* _UTILS_H */
