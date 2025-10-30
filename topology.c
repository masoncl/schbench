/*
 * topology.c - CPU topology detection for schbench
 *
 * Detects CPU topology from sysfs to support pinning threads to specific
 * dies/chiplets on modern AMD CPUs
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sched.h>
#include <unistd.h>

#include "topology.h"

static int read_int_from_file(const char *path)
{
	FILE *fp;
	int value = -1;

	fp = fopen(path, "r");
	if (fp) {
		fscanf(fp, "%d", &value);
		fclose(fp);
	}
	return value;
}

void free_topology(struct cpu_topology *topo)
{
	if (!topo)
		return;

	if (topo->dies) {
		free(topo->dies);
		topo->dies = NULL;
	}
	topo->num_dies = 0;
}

int detect_topology(struct cpu_topology *topo)
{
	DIR *dir;
	struct dirent *entry;
	char path[256];
	int cpu_id;
	int die_id;
	int i;

	memset(topo, 0, sizeof(*topo));

	/* First pass: count CPUs and find max die ID */
	dir = opendir("/sys/devices/system/cpu");
	if (!dir) {
		perror("Failed to open /sys/devices/system/cpu");
		return -1;
	}

	int max_die_id = -1;
	CPU_ZERO(&topo->all_cpus);

	while ((entry = readdir(dir)) != NULL) {
		if (sscanf(entry->d_name, "cpu%d", &cpu_id) == 1) {
			CPU_SET(cpu_id, &topo->all_cpus);

			/* Check if this CPU is online */
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/online",
				 cpu_id);
			int online = read_int_from_file(path);
			/* CPU0 doesn't have online file, assume it's online */
			if (online == -1 && cpu_id == 0)
				online = 1;
			if (online != 1)
				continue;

			/* Read die_id for this CPU */
			snprintf(
				path, sizeof(path),
				"/sys/devices/system/cpu/cpu%d/topology/die_id",
				cpu_id);
			die_id = read_int_from_file(path);

			/* Fallback to package_id if die_id doesn't exist */
			if (die_id == -1) {
				snprintf(
					path, sizeof(path),
					"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
					cpu_id);
				die_id = read_int_from_file(path);
			}

			if (die_id > max_die_id)
				max_die_id = die_id;
		}
	}
	closedir(dir);

	if (max_die_id < 0) {
		fprintf(stderr, "Failed to detect CPU topology\n");
		return -1;
	}

	/* Allocate die structures */
	topo->num_dies = max_die_id + 1;
	topo->dies = calloc(topo->num_dies, sizeof(struct die_info));
	if (!topo->dies) {
		perror("Failed to allocate die structures");
		return -1;
	}

	/* Initialize die structures */
	for (i = 0; i < topo->num_dies; i++) {
		topo->dies[i].die_id = i;
		CPU_ZERO(&topo->dies[i].cpus);
	}

	/* Second pass: populate die CPU sets */
	dir = opendir("/sys/devices/system/cpu");
	if (!dir) {
		perror("Failed to open /sys/devices/system/cpu");
		free_topology(topo);
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (sscanf(entry->d_name, "cpu%d", &cpu_id) == 1) {
			/* Check if this CPU is online */
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/online",
				 cpu_id);
			int online = read_int_from_file(path);
			if (online == -1 && cpu_id == 0)
				online = 1;
			if (online != 1)
				continue;

			/* Read die_id for this CPU */
			snprintf(
				path, sizeof(path),
				"/sys/devices/system/cpu/cpu%d/topology/die_id",
				cpu_id);
			die_id = read_int_from_file(path);

			/* Fallback to package_id if die_id doesn't exist */
			if (die_id == -1) {
				snprintf(
					path, sizeof(path),
					"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
					cpu_id);
				die_id = read_int_from_file(path);
			}

			if (die_id >= 0 && die_id < topo->num_dies) {
				CPU_SET(cpu_id, &topo->dies[die_id].cpus);
				topo->dies[die_id].num_cpus++;
			}
		}
	}
	closedir(dir);

	return 0;
}

void print_topology(struct cpu_topology *topo)
{
	int i, j;
	int first;

	fprintf(stderr, "CPU Topology: %d dies detected\n", topo->num_dies);

	for (i = 0; i < topo->num_dies; i++) {
		fprintf(stderr, "  Die %d: ", i);
		first = 1;

		/* Print CPU ranges in a compact format */
		int range_start = -1;
		int last_cpu = -1;

		for (j = 0; j < CPU_SETSIZE; j++) {
			if (CPU_ISSET(j, &topo->dies[i].cpus)) {
				if (range_start == -1) {
					range_start = j;
				}
				last_cpu = j;
			} else if (range_start != -1) {
				/* End of a range */
				if (!first)
					fprintf(stderr, ",");
				if (range_start == last_cpu)
					fprintf(stderr, "%d", range_start);
				else
					fprintf(stderr, "%d-%d", range_start,
						last_cpu);
				first = 0;
				range_start = -1;
			}
		}

		/* Handle the last range if it extends to the end */
		if (range_start != -1) {
			if (!first)
				fprintf(stderr, ",");
			if (range_start == last_cpu)
				fprintf(stderr, "%d", range_start);
			else
				fprintf(stderr, "%d-%d", range_start, last_cpu);
		}

		fprintf(stderr, " (%d CPUs)\n", topo->dies[i].num_cpus);
	}
}

void print_thread_cpus(const char *prefix, int index, cpu_set_t *cpus)
{
	int j;
	int first = 1;
	int range_start = -1;
	int last_cpu = -1;

	fprintf(stderr, "%s %d: cpus ", prefix, index);

	for (j = 0; j < CPU_SETSIZE; j++) {
		if (CPU_ISSET(j, cpus)) {
			if (range_start == -1) {
				range_start = j;
			}
			last_cpu = j;
		} else if (range_start != -1) {
			/* End of a range */
			if (!first)
				fprintf(stderr, ",");
			if (range_start == last_cpu)
				fprintf(stderr, "%d", range_start);
			else
				fprintf(stderr, "%d-%d", range_start, last_cpu);
			first = 0;
			range_start = -1;
		}
	}

	/* Handle the last range if it extends to the end */
	if (range_start != -1) {
		if (!first)
			fprintf(stderr, ",");
		if (range_start == last_cpu)
			fprintf(stderr, "%d", range_start);
		else
			fprintf(stderr, "%d-%d", range_start, last_cpu);
	}

	fprintf(stderr, "\n");
}
