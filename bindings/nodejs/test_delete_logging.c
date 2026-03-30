#include <stdio.h>

__attribute__((constructor))
void preload() {
    fprintf(stderr, "DEBUG: Module loaded\n");
    fflush(stderr);
}
