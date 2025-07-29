/*
 * topology.h - CPU topology detection header
 */
#ifndef _TOPOLOGY_H
#define _TOPOLOGY_H

#include <sched.h>

struct die_info {
	int die_id;
	int num_cpus;
	cpu_set_t cpus;
};

struct cpu_topology {
	int num_dies;
	struct die_info *dies;
	cpu_set_t all_cpus;
};

/* Detect CPU topology from sysfs */
int detect_topology(struct cpu_topology *topo);

/* Free topology structures */
void free_topology(struct cpu_topology *topo);

/* Print topology information */
void print_topology(struct cpu_topology *topo);

/* Print compact CPU list for a thread */
void print_thread_cpus(const char *prefix, int index, cpu_set_t *cpus);

#endif /* _TOPOLOGY_H */
