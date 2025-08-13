/*
 * options.h - Command line option parsing
 */
#ifndef _OPTIONS_H
#define _OPTIONS_H

#include "schbench.h"

/* Function declarations */
void parse_options(int ac, char **av);
void print_usage(void);
int parse_cpuset(const char *str, cpu_set_t *cpuset);

#endif /* _OPTIONS_H */
