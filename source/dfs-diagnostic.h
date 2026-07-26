#ifndef NUTRIMATIC_DFS_DIAGNOSTIC_H
#define NUTRIMATIC_DFS_DIAGNOSTIC_H

#include <stdio.h>

void dfs_reset_diagnostic_clock();
void dfs_diagnostic(FILE* stream, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
