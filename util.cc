
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

void pin_to_cpu_core(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
        perror("sched_setaffinity");
    }
}

void set_priority(int priority) {
    int result = setpriority(PRIO_PROCESS, 0, priority);

    if (result == -1) {
        perror("Failed to set priority"); // Print error message if setpriority fails
        // Check errno for specific error details
        if (errno == EACCES) {
            fprintf(stderr, "Permission denied: You likely need root privileges to set such a high priority.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr, "Invalid priority value: The system might not support -20.\n");
        }
        // exit(1); // Indicate an error
    } else {
        printf("Priority set to highest possible (-20 nice value).\n");
    }
}
