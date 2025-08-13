/*
 * utils.c - Utility functions implementation
 */
#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <time.h>
#include "utils.h"
#include "schbench.h"

void tvsub(struct timeval *tdiff, struct timeval *t1, struct timeval *t0)
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

unsigned long get_sys_tid(void)
{
	return syscall(SYS_gettid);
}

/*
 * read the schedstats for a process and return the average scheduling delay
 * in nanoseconds
 */
unsigned long long read_sched_delay(pid_t tid)
{
	unsigned long long runqueue_ns = 0;
	unsigned long long running_ns = 0;
	unsigned long long nr_scheduled = 0;
	char path[96];

	snprintf(path, sizeof(path), "/proc/%d/schedstat", tid);

	FILE *fp = fopen(path, "r");
	if (!fp) {
		/* this can happen during final stats print at exit */
		return 0;
	}

	/*
	 * proc_pid_schedstat() in the kernel prints:
	 * runtime, delay, pcount
	 */
	int ret = fscanf(fp, "%llu %llu %llu", &running_ns, &runqueue_ns,
			 &nr_scheduled);
	fclose(fp);

	if (ret != 3) {
		fprintf(stderr, "Failed to parse %s\n", path);
		exit(1);
	}

	return runqueue_ns / nr_scheduled;
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

int find_nth_set_bit(const cpu_set_t *set, int n)
{
	int count = 0;
	for (int i = 0; i < CPU_SETSIZE; ++i) {
		if (CPU_ISSET(i, set)) {
			if (count == n)
				return i; // Return the CPU index of the n'th set bit
			++count;
		}
	}
	return -1; // Not found
}

void pin_worker_cpus(cpu_set_t *worker_cpus)
{
	int ret;
	pthread_t thread = pthread_self();
	ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), worker_cpus);
	if (ret) {
		fprintf(stderr, "unable to set CPU affinity\n");
	}
}

void pin_message_cpu(int index, cpu_set_t *possible_cpus)
{
	cpu_set_t cpuset;
	int ret;
	
	CPU_ZERO(&cpuset);
	int num_possible = CPU_COUNT(possible_cpus);
	int cpu_to_set = index % num_possible;

	if (pin_mode == PIN_MODE_CCX)
		cpu_to_set = 0;

	cpu_to_set = find_nth_set_bit(possible_cpus, cpu_to_set);
	CPU_SET(cpu_to_set, &cpuset);

	pthread_t thread = pthread_self();
	ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (ret) {
		fprintf(stderr, "unable to set CPU affinity for message thread %d\n", index);
		exit(1);
	}
	fprintf(stderr, "Pinning to message thread index %d cpu %d\n", 
			index, find_nth_set_bit(&cpuset, 0));
}

/*
 * multiply two matrices in a naive way to emulate some cache footprint
 */
void do_some_math(struct thread_data *thread_data)
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

pthread_mutex_t *lock_this_cpu(void)
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
void do_work(struct thread_data *td)
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

char *escape_string(char *str)
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

void chomp(char *buf)
{
	size_t max = strlen(buf);
	size_t index = max - 1;

	if (max == 0)
		return;

	while (index && isspace(buf[index])) index--;
	index++;
	buf[index] = '\0';
}

void print_sched_ext_info(FILE *fp)
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

void write_json_header(FILE *fp, char **argv, int argc)
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

void write_json_footer(FILE *fp)
{
	fprintf(fp, "}}");
	fflush(fp);
}

static char *units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB", NULL};

double pretty_size(double number, char **str)
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
