/*
 * stats.h - Statistics and latency histogram functions
 */
#ifndef _STATS_H
#define _STATS_H

#include "schbench.h"

/* Latency percentile definitions */
#define PLIST_20 (1 << 0)
#define PLIST_50 (1 << 1)
#define PLIST_90 (1 << 2)
#define PLIST_99 (1 << 3)
#define PLIST_99_INDEX 3
#define PLIST_999 (1 << 4)

#define PLIST_FOR_LAT (PLIST_50 | PLIST_90 | PLIST_99 | PLIST_999)
#define PLIST_FOR_RPS (PLIST_20 | PLIST_50 | PLIST_90)

extern double plist[PLAT_LIST_MAX];

/* Function declarations */
void combine_stats(struct stats *d, struct stats *s);
void add_lat(struct stats *s, unsigned int us);
void show_latencies(struct stats *s, char *label, char *units,
		    unsigned long long runtime, unsigned long mask,
		    unsigned long star);
void write_json_stats(FILE *fp, struct stats *s, char *label);

/* Internal histogram functions */
unsigned int plat_val_to_idx(unsigned int val);
unsigned int plat_idx_to_val(unsigned int idx);
unsigned int calc_percentiles(unsigned int *io_u_plat, unsigned long nr,
			      unsigned int **output,
			      unsigned long **output_counts);

#endif /* _STATS_H */
