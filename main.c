// Argument handling
#include <argp.h>

// errno
#include <errno.h>

// sigemptyset, sigaddset, sigprocmask, kill
#include <signal.h>

// strerror
#include <string.h>

// sscanf, fprintf
#include <stdio.h>

// exit
#include <stdlib.h>

// pidfd_open
#include <sys/pidfd.h>

// pidfd_send_signal
#include <sys/signalfd.h>

// waitid
#include <sys/wait.h>

// close, read
#include <unistd.h>

//
#include "sfork.h"
#include "sepoll.h"
#include "server.h"

// *********************************************************************
// Command line arguments
// *********************************************************************

// Constants for arguments
enum arg_keys
{
	KEY_DIRECTORY = 'd',
	KEY_PORT = 'p',
	KEY_WORKERS = 'w'
};

// Program arguments
struct arguments
{
	char* directory;
	unsigned short port;
	int numWorkers;
};

// options vector
static struct argp_option argp_options[] =
{
	{"directory",	KEY_DIRECTORY,	"STRING",	0,	"Location to serve files from. Defaults to "},
	{"port",		KEY_PORT,		"NUMBER",	0,	"Network port. Defaults to 70."},
	{"workers",		KEY_WORKERS,	"NUMBER",	0,	"Number of worker processes. Defaults to 4. Setting to 1 results in no forking."},
	{0}
};

// documentation string
static char argp_doc[] = "";

// argp globals (these must have these names)
const char* argp_program_version = "0.1";
const char* argp_program_bug_address = "<contact@sarahwatt.ca>";

//
static error_t argp_parse_options(int key, char* arg, struct argp_state* state)
{
	struct arguments* args = state->input;

	switch (key)
	{
	case KEY_DIRECTORY:
		args->directory = arg;
		break;
	case KEY_PORT:
		sscanf(arg, "%hu", &args->port);
		break;
	case KEY_WORKERS:
		sscanf(arg, "%i", &args->numWorkers);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	
	return 0;
}

// *********************************************************************
//
// *********************************************************************

#define MAX_WORKERS 256

struct worker
{
	int number;
	pid_t pid;
	int pidfd;
};

struct supervisor
{
	struct worker workers[MAX_WORKERS];
	int numWorkers;
	int activeWorkers;
	int sigfd;
	struct sepoll_loop* loop;
};

static void sigfd_event(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct supervisor* supervisor = userdata1;
	
	while (1)
	{
		struct signalfd_siginfo siginfo;
		
		if (read(fd, &siginfo, sizeof(struct signalfd_siginfo)) != sizeof(struct signalfd_siginfo))
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}
			else
			{
				fprintf(stderr, "S - Warning: Cannot read from signalfd: %s\n", strerror(errno));
				return;
			}
		}
		
		switch (siginfo.ssi_signo)
		{
		case SIGTERM:
			for (int i = 0; i < supervisor->numWorkers; i++)
			{
				if (supervisor->workers[i].pid >= 0)
				{
					if (pidfd_send_signal(supervisor->workers[i].pidfd, SIGTERM, NULL, 0) < 0)
					{
						fprintf(stderr, "S - Warning: Cannot send kill signal via pidfd: %s\n", strerror(errno));
						
						if (kill(supervisor->workers[i].pid, SIGTERM) < 0)
						{
							fprintf(stderr, "S - Warning: Kill() failed as well: %s\n", strerror(errno));
						}
					}
				}
			}
			
			break;
		}
	}
}

static void pidfd_event(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct supervisor* supervisor = userdata1;
	struct worker* worker = userdata2;
	
	siginfo_t siginfo;
	
	if (waitid(P_PIDFD, (id_t)fd, &siginfo, WEXITED) < 0)
	{
		fprintf(stderr, "S - Worker PID %i exited but waitid failed: %s\n", worker->pid, strerror(errno));
	}
	else
	{
		fprintf(stderr, "S - Worker PID %i exited with status %i\n", worker->pid, siginfo.si_status);
	}

	sepoll_remove(supervisor->loop, fd, 1);
	
	//close(fd);
	
	worker->pidfd = -1;
	
	supervisor->activeWorkers--;
	
	if (supervisor->activeWorkers == 0)
	{
		sepoll_exit(supervisor->loop);
	}
}

// *********************************************************************
//
// *********************************************************************
int main(int argc, char* argv[])
{
	// argp parser options
	struct argp argp_parser = {argp_options, argp_parse_options, 0, argp_doc};
	
	// Default argument values
	struct arguments args;
	args.directory = ".";
	args.port = 70;
	args.numWorkers = 4;

	// Parse arguments
	argp_parse(&argp_parser, argc, argv, 0, 0, &args);
	
	//
	if (args.numWorkers < 1)
	{
		fprintf(stderr, "S - Invalid number of worker processes - must be at least 1\n");
		exit(EXIT_FAILURE);
	}
	
	if (args.numWorkers > MAX_WORKERS)
	{
		fprintf(stderr, "S - Invalid number of worker processes - must be no greater than %i\n", MAX_WORKERS);
		exit(EXIT_FAILURE);
	}
	
	fprintf(stderr, "S - Spawning %i workers\n", args.numWorkers);
	
	//
	fprintf(stderr, "S - Listening on port %hu\n", args.port);
	
	//
	struct server_params params;
	params.directory = args.directory;
	params.port = args.port;
	params.maxClients = 4096;
	params.indexFile = "index";
	params.timeout = 10;
	
	// 
	struct supervisor supervisor;
	
	// Spawn worker processes
	pid_t pid;
	
	for (int i = 0; i < args.numWorkers; i++)
	{
		int pidfd;
		
		pid = sfork(&pidfd);
	
		if (pid == 0) // Worker
		{
			break;
		}
		else if (pid < 0) // Error
		{
			fprintf(stderr, "S - Error: Cannot fork off worker process %i - %s\n", i, strerror(errno));
			exit(EXIT_FAILURE);
		}
		else // Parent
		{
			fprintf(stderr, "S - Spawned worker process %i (PID %i)\n", i, pid);
			
			supervisor.workers[i].number = i;
			supervisor.workers[i].pid = pid;
			supervisor.workers[i].pidfd = pidfd;
		}
	}

	// Tasks for each process
	if (pid == 0) // Worker task
	{
		int devnull = open("/dev/null", O_RDONLY);
		
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDOUT_FILENO);
		
		close(devnull);
		
		int retval = doserver(&params);
		
		if (retval < 0)
		{
			exit(EXIT_FAILURE);
		}
		
		exit(EXIT_SUCCESS);
	}
	else // Parent task
	{
		supervisor.numWorkers = args.numWorkers;
		supervisor.activeWorkers = args.numWorkers;
		
		//
		sigset_t mask;
		
		sigemptyset(&mask);
		sigaddset(&mask, SIGTERM);
		
		if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		{
			fprintf(stderr, "%i - Error: Cannot block signals: %s\n", getpid(), strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		supervisor.sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
		
		if (supervisor.sigfd < 0)
		{
			fprintf(stderr, "%i - Error: Cannot open signalfd: %s\n", getpid(), strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		fprintf(stderr, "S - All workers spawned\n");
		
		supervisor.loop = sepoll_create(args.numWorkers + 1);
		
		sepoll_add(supervisor.loop, supervisor.sigfd, EPOLLIN | EPOLLET, sigfd_event, &supervisor, NULL);
		
		for (int i = 0; i < args.numWorkers; i++)
		{
			sepoll_add(supervisor.loop, supervisor.workers[i].pidfd, EPOLLIN, pidfd_event, &supervisor, &supervisor.workers[i]);
		}
		
		sepoll_enter(supervisor.loop);
		
		sepoll_destroy(supervisor.loop);
		
		close(supervisor.sigfd);
		
		fprintf(stderr, "S - All workers exited\n");
		
		exit(EXIT_SUCCESS);
	}
}
