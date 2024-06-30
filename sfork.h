#pragma once

// CLONE_*
#include <linux/sched.h>

// pid_t
#include <sys/types.h>

pid_t sfork(int* pidfd, __u64 flags);
