#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

pid_t sfork(int* pidfd)
{
	return (pid_t)syscall(SYS_clone, CLONE_PIDFD, NULL, pidfd, NULL, 0);
}