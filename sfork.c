#define _GNU_SOURCE
#include <linux/sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// This variant of fork returns both a pid and a pidfd which is marked close-on-exec
// It does not perform any of the additional tasks that the glibc fork does, because
// they are not relevant to how I am using it. However, because of that, it is not
// strictly a drop-in upgrade for fork.
pid_t sfork(int* pidfd, __u64 flags)
{
	struct clone_args args =
	{
		.flags = CLONE_PIDFD | flags,
		.pidfd = (__u64)pidfd,
		.exit_signal = SIGCHLD
	};
	
	return (pid_t)syscall(SYS_clone3, &args, sizeof(struct clone_args));
}
