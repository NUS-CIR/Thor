#include <stdio.h>
#include <stdlib.h>

// Stub implementation of exit_function for test programs
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
    if (s != NULL) {
        fprintf(stderr, "%s:%d %s() Error: %s\n", file, line, function, s);
    }
    abort();
}
