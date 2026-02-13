/* test_migrate.c - Simple test for NUMA migration functionality */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>
#include <sched.h>

/* Include the migration module */
#include "src/zmalloc.h"
#include "src/numa_migrate.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    printf("=== NUMA Migration Test Program ===\n\n");
    
    /* Check NUMA availability */
    if (numa_available() == -1) {
        printf("NUMA is not available on this system\n");
        return 1;
    }
    
    int num_nodes = numa_max_node() + 1;
    printf("NUMA available with %d node(s)\n", num_nodes);
    
    /* Initialize zmalloc */
    zmalloc_set_oom_handler(NULL);
    
    /* Initialize migration module */
    if (numa_migrate_init() != 0) {
        printf("Failed to initialize migration module\n");
        return 1;
    }
    printf("Migration module initialized\n");
    
    /* Run the built-in test */
    printf("\nRunning migration test...\n");
    int result = numa_migrate_test();
    
    /* Cleanup */
    numa_migrate_cleanup();
    
    if (result == 0) {
        printf("\nAll tests PASSED!\n");
        return 0;
    } else {
        printf("\nTests FAILED!\n");
        return 1;
    }
}
