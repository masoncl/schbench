/*
 * stats.c - Statistics and latency histogram implementation
 */
#include <string.h>
#include <math.h>
#include "stats.h"

double plist[PLAT_LIST_MAX] = { 20.0, 50.0, 90.0, 99.0, 99.9 };

/* mr axboe's magic latency histogram */
unsigned int plat_val_to_idx(unsigned int val)
{
	unsigned int msb, error_bits, base, offset;

	/* Find MSB starting from bit 0 */
	if (val == 0)
		msb = 0;
	else
		msb = sizeof(val)*8 - __builtin_clz(val) - 1;

	/*
	 * MSB <= (PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index
	 */
	if (msb <= PLAT_BITS)
		return val;

	/* Compute the number of error bits to discard*/
	error_bits = msb - PLAT_BITS;

	/* Compute the number of buckets before the group */
	base = (error_bits + 1) << PLAT_BITS;

	/*
	 * Discard the error bits and apply the mask to find the
	 * index for the buckets in the group
	 */
	offset = (PLAT_VAL - 1) & (val >> error_bits);

	/* Make sure the index does not exceed (array size - 1) */
	return (base + offset) < (PLAT_NR - 1) ?
		(base + offset) : (PLAT_NR - 1);
}

/*
 * Convert the given index of the bucket array to the value
 * represented by the bucket
 */
unsigned int plat_idx_to_val(unsigned int idx)
{
	unsigned int error_bits, k, base;

	if (idx >= PLAT_NR) {
		fprintf(stderr, "idx %u is too large\n", idx);
		exit(1);
	}

	/* MSB <= (PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index */
	if (idx < (PLAT_VAL << 1))
		return idx;

	/* Find the group and compute the minimum value of that group */
	error_bits = (idx >> PLAT_BITS) - 1;
	base = 1 << (error_bits + PLAT_BITS);

	/* Find its bucket number of the group */
	k = idx % PLAT_VAL;

	/* Return the mean of the range of the bucket */
	return base + ((k + 0.5) * (1 << error_bits));
}

unsigned int calc_percentiles(unsigned int *io_u_plat, unsigned long nr,
			     unsigned int **output,
			     unsigned long **output_counts)
{
	unsigned long sum = 0;
	unsigned int len, i, j = 0;
	unsigned int oval_len = 0;
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned long last = 0;
	int is_last;

	len = 0;
	while (len < PLAT_LIST_MAX && plist[len] != 0.0)
		len++;

	if (!len)
		return 0;

	/*
	 * Calculate bucket values, note down max and min values
	 */
	is_last = 0;
	for (i = 0; i < PLAT_NR && !is_last; i++) {
		sum += io_u_plat[i];
		while (sum >= (plist[j] / 100.0 * nr)) {
			if (j == oval_len) {
				oval_len += 100;
				ovals = realloc(ovals, oval_len * sizeof(unsigned int));
				ocounts = realloc(ocounts, oval_len * sizeof(unsigned long));
			}

			ovals[j] = plat_idx_to_val(i);
			ocounts[j] = sum;
			is_last = (j == len - 1);
			if (is_last)
				break;
			j++;
		}
	}

	for (i = 1; i < len; i++) {
		last += ocounts[i - 1];
		ocounts[i] -= last;
	}
	*output = ovals;
	*output_counts = ocounts;
	return len;
}

void show_latencies(struct stats *s, char *label, char *units,
		   unsigned long long runtime, unsigned long mask,
		   unsigned long star)
{
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned int len, i;

	len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
	if (len) {
		fprintf(stderr, "%s percentiles (%s) runtime %llu (s) (%lu total samples)\n",
			label, units, runtime, s->nr_samples);
		for (i = 0; i < len; i++) {
			unsigned long bit = 1 << i;
			if (!(mask & bit))
				continue;
			fprintf(stderr, "\t%s%2.1fth: %-10u (%lu samples)\n",
				bit == star ? "* " : "  ",
				plist[i], ovals[i], ocounts[i]);
		}
	}

	if (ovals)
		free(ovals);
	if (ocounts)
		free(ocounts);

	fprintf(stderr, "\t  min=%u, max=%u\n", s->min, s->max);
}

void write_json_stats(FILE *fp, struct stats *s, char *label)
{
	unsigned int *ovals = NULL;
	unsigned long *ocounts = NULL;
	unsigned int len, i;

	len = calc_percentiles(s->plat, s->nr_samples, &ovals, &ocounts);
	if (len) {
		for (i = 0; i < len; i++) {
			if (i)
				fprintf(fp, ", ");
			fprintf(fp, "\"%s_pct%2.1f\": %u", label, plist[i], ovals[i]);
		}
		fprintf(fp, ", \"%s_min\": %u,", label, s->min);
		fprintf(fp, "\"%s_max\": %u", label, s->max);
	}

	if (ovals)
		free(ovals);
	if (ocounts)
		free(ocounts);
}

/* fold latency info from s into d */
void combine_stats(struct stats *d, struct stats *s)
{
	int i;
	for (i = 0; i < PLAT_NR; i++)
		d->plat[i] += s->plat[i];
	d->nr_samples += s->nr_samples;
	if (s->max > d->max)
		d->max = s->max;
	if (d->min == 0 || s->min < d->min)
		d->min = s->min;
}

/* record a latency result into the histogram */
void add_lat(struct stats *s, unsigned int us)
{
	int lat_index = 0;

	if (us > s->max)
		s->max = us;
	if (s->min == 0 || us < s->min)
		s->min = us;

	lat_index = plat_val_to_idx(us);
	__sync_fetch_and_add(&s->plat[lat_index], 1);
	__sync_fetch_and_add(&s->nr_samples, 1);
}
