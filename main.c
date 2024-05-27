// Argument handling
#include <argp.h>

// errno
#include <errno.h>

// open
#include <fcntl.h>

// sigemptyset, sigaddset, sigprocmask
#include <signal.h>

// bool
#include <stdbool.h>

// sscanf, fprintf
#include <stdio.h>

// exit, on_exit, malloc, calloc, free
#include <stdlib.h>

// strerror
#include <string.h>

// pidfd_send_signal
#include <sys/pidfd.h>

// signalfd
#include <sys/signalfd.h>

// waitid
#include <sys/wait.h>

// close, read, getpid, dup2
#include <unistd.h>

// My stuff
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
	KEY_HOSTNAME = 'h',
	KEY_INDEXFILE = 'i',
	KEY_MAXCLIENTS = 'm',
	KEY_PORT = 'p',
	KEY_TIMEOUT = 't',
	KEY_WORKERS = 'w'
};

// Program arguments
struct arguments
{
	const char* directory;
	const char* hostname;
	const char* indexfile;
	unsigned int maxClients;
	unsigned short port;
	unsigned int timeout;
	unsigned int numWorkers;
};

// options vector
static struct argp_option argp_options[] =
{
	{"directory",	KEY_DIRECTORY,	"STRING",	0,	"Location to serve files from. Defaults to ./gopherroot"},
	{"hostname",	KEY_HOSTNAME,	"STRING",	0,	"Externally-accessible hostname of server for generation of gophermaps. Defaults to localhost"},
	{"indexfile",	KEY_INDEXFILE,	"STRING",	0,	"Default file to serve from a blank path or path referencing a directory. Defaults to .gophermap"},
	{"maxclients",	KEY_MAXCLIENTS,	"NUMBER",	0,	"Maximum simultaneous clients per worker process. Defaults to 4096"},
	{"port",		KEY_PORT,		"NUMBER",	0,	"Network port. Defaults to 70"},
	{"timeout",		KEY_TIMEOUT,	"NUMBER",	0,	"Time in seconds before booting inactive client. Defaults to 10"},
	{"workers",		KEY_WORKERS,	"NUMBER",	0,	"Number of worker processes. Defaults to 1"},
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
	case KEY_HOSTNAME:
		args->hostname = arg;
		break;
	case KEY_INDEXFILE:
		args->indexfile = arg;
		break;
	case KEY_MAXCLIENTS:
		sscanf(arg, "%u", &args->maxClients);
		break;
	case KEY_PORT:
		sscanf(arg, "%hu", &args->port);
		break;
	case KEY_TIMEOUT:
		sscanf(arg, "%u", &args->timeout);
		break;
	case KEY_WORKERS:
		sscanf(arg, "%u", &args->numWorkers);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	
	return 0;
}

// *********************************************************************
// Server state definitions
// *********************************************************************

struct worker_t
{
	unsigned int number;
	pid_t pid;
	int pidfd;
};

struct supervisor_t
{
	pid_t pid;
	struct worker_t* workers;
	unsigned int numWorkers;
	unsigned int activeWorkers;
	int sigfd;
	struct sepoll_loop* loop;
};

// *********************************************************************
// Cleanup functions
// *********************************************************************

static void signal_all_workers(struct supervisor_t* supervisor, int sig, bool close_pidfd)
{
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		if (supervisor->workers[i].pidfd >= 0)
		{
			if (pidfd_send_signal(supervisor->workers[i].pidfd, sig, NULL, 0) < 0)
			{
				fprintf(stderr, "S - Error: Cannot send signal to child via pidfd: %s\n", strerror(errno));
			}
			
			if (close_pidfd)
			{
				close(supervisor->workers[i].pidfd);
			}
		}
	}
}

static void cleanup(int code, void* arg)
{
	struct supervisor_t* supervisor = arg;
	
	if (supervisor->workers != NULL)
	{
		if (code == EXIT_FAILURE)
		{
			if (supervisor->pid == getpid())
			{
				signal_all_workers(supervisor, SIGKILL, true);
			}
		}
		
		free(supervisor->workers);
	}
	
	if (supervisor->sigfd >= 0)
	{
		close(supervisor->sigfd);
	}
	
	if (supervisor->loop != NULL)
	{
		sepoll_destroy(supervisor->loop);
	}
	
	free(supervisor);
}

// *********************************************************************
// Server event handlers
// *********************************************************************

// signalfd event handler
static void sigfd_event(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct supervisor_t* supervisor = userdata1;
	
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
				fprintf(stderr, "S - Error: Cannot read from signalfd: %s\n", strerror(errno));
				return;
			}
		}
		
		switch (siginfo.ssi_signo)
		{
		case SIGTERM:
			fprintf(stderr, "S - Received SIGTERM, sending SIGTERM to children\n");
			
			signal_all_workers(supervisor, SIGTERM, false);
			
			break;
		}
	}
}

// pidfd event handler
static void pidfd_event(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct supervisor_t* supervisor = userdata1;
	struct worker_t* worker = userdata2;
	
	siginfo_t siginfo;
	
	if (waitid(P_PIDFD, (id_t)fd, &siginfo, WEXITED) < 0)
	{
		fprintf(stderr, "S - Worker PID %i exited but waitid failed: %s\n", worker->pid, strerror(errno));
	}
	else
	{
		fprintf(stderr, "S - Worker PID %i exited with status %i\n", worker->pid, siginfo.si_status);
	}

	sepoll_remove(supervisor->loop, fd);
	close(fd);
	
	worker->pidfd = -1;
	
	supervisor->activeWorkers--;
	
	if (supervisor->activeWorkers == 0)
	{
		sepoll_exit(supervisor->loop);
	}
}

// *********************************************************************
// Main
// *********************************************************************
int main(int argc, char* argv[])
{
	// argp parser options
	struct argp argp_parser = {argp_options, argp_parse_options, 0, argp_doc};
	
	// Default argument values
	struct arguments args;
	args.directory = "./gopherroot";
	args.hostname = "localhost";
	args.indexfile = ".gophermap";
	args.maxClients = 4096;
	args.port = 70;
	args.timeout = 10;
	args.numWorkers = 1;

	// Parse arguments
	argp_parse(&argp_parser, argc, argv, 0, 0, &args);
	
	// Report arguments
	fprintf(stderr, "S - Serving files from %s\n", args.directory);
	fprintf(stderr, "S - Hostname is %s\n", args.hostname);
	fprintf(stderr, "S - Index filename is %s\n", args.indexfile);
	fprintf(stderr, "S - Maximum number of clients is %u\n", args.maxClients);
	fprintf(stderr, "S - Listening on port %hu\n", args.port);
	fprintf(stderr, "S - Timeout is %u seconds\n", args.timeout);
	fprintf(stderr, "S - Spawning %u workers\n", args.numWorkers);
	
	// Copy arguments to server parameters
	// Why do it like this? Just in case they are parsed from a configuration file in the future instead of the command line
	struct server_params params;
	params.hostname = args.hostname;
	params.directory = args.directory;
	params.port = args.port;
	params.maxClients = args.maxClients;
	params.indexfile = args.indexfile;
	params.timeout = args.timeout;
	
	// Where we're going we only need stderr
	int devnull = open("/dev/null", O_RDWR);
	
	if (devnull < 0)
	{
		fprintf(stderr, "S - Error: Cannot open /dev/null: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	if (dup2(devnull, STDIN_FILENO) < 0 || dup2(devnull, STDOUT_FILENO) < 0)
	{
		close(devnull);
		fprintf(stderr, "S - Error: Cannot dup2 /dev/null over standard file descriptors: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	close(devnull);
	
	// Allocate and set up supervisor
	struct supervisor_t* supervisor = malloc(sizeof(struct supervisor_t));
	
	if (supervisor == NULL)
	{
		fprintf(stderr, "S - Error: Cannot allocate memory for supervisor: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	supervisor->pid = getpid();
	supervisor->numWorkers = args.numWorkers;
	supervisor->sigfd = -1;
	supervisor->loop = NULL;
	
	on_exit(cleanup, supervisor);
	
	// Allocate and set up workers
	supervisor->workers = calloc(supervisor->numWorkers, sizeof(struct worker_t));
	
	if (supervisor->workers == NULL)
	{
		fprintf(stderr, "S - Error: Cannot allocate memory for workers: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		supervisor->workers[i].pidfd = -1;
	}
	
	// Spawn worker processes
	pid_t pid;
	
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		int pidfd;
		
		pid = sfork(&pidfd);
	
		if (pid == 0) // Worker
		{
			int retval = doserver(&params);
			
			if (retval < 0)
			{
				exit(EXIT_FAILURE);
			}
			
			exit(EXIT_SUCCESS);
		}
		else if (pid < 0) // Error
		{
			fprintf(stderr, "S - Error: Cannot fork worker process %u - %s\n", i, strerror(errno));
			exit(EXIT_FAILURE);
		}
		else // Parent
		{
			fprintf(stderr, "S - Spawned worker process %u (PID %i)\n", i, pid);
			
			supervisor->workers[i].number = i;
			supervisor->workers[i].pid = pid;
			supervisor->workers[i].pidfd = pidfd;
		}
	}
	
	// Supervisor task begins here
	fprintf(stderr, "S - All workers spawned\n");
	
	supervisor->activeWorkers = supervisor->numWorkers;
	
	// Set up signals and the signalfd
	sigset_t mask;
	
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
	{
		fprintf(stderr, "S - Error: Cannot block signals: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	supervisor->sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
	
	if (supervisor->sigfd < 0)
	{
		fprintf(stderr, "S - Error: Cannot open signalfd: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Create event loop with enough room for each worker and the signalfd
	supervisor->loop = sepoll_create((int)supervisor->numWorkers + 1);
	
	sepoll_add(supervisor->loop, supervisor->sigfd, EPOLLIN | EPOLLET, sigfd_event, supervisor, NULL);
	
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		sepoll_add(supervisor->loop, supervisor->workers[i].pidfd, EPOLLIN, pidfd_event, supervisor, &supervisor->workers[i]);
	}
	
	// Event loop doesn't exit until all children exit
	sepoll_enter(supervisor->loop);
	
	fprintf(stderr, "S - All workers exited\n");
	
	exit(EXIT_SUCCESS);
}
