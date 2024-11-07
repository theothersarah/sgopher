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

// pidfd_send_signal
#include <sys/pidfd.h>

// signalfd
#include <sys/signalfd.h>

// waitid
#include <sys/wait.h>

// close, read, dup2
#include <unistd.h>

// event loop functions
#include "sepoll.h"

// server entry function and parameters
#include "server.h"

// sfork
#include "sfork.h"

// *********************************************************************
// Command line arguments
// *********************************************************************

// Constants for arguments
enum arg_keys_t
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
struct args_t
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
	{"directory",	KEY_DIRECTORY,	"STRING",	0,	"Location to serve files from (default ./gopherroot)"},
	{"hostname",	KEY_HOSTNAME,	"STRING",	0,	"Externally-accessible hostname of server, used for generation of gophermaps (default localhost)"},
	{"indexfile",	KEY_INDEXFILE,	"STRING",	0,	"Default file to serve from a blank path or path referencing a directory (default .gophermap)"},
	{"maxclients",	KEY_MAXCLIENTS,	"NUMBER",	0,	"Maximum simultaneous clients per worker process (default 1000 clients)"},
	{"port",		KEY_PORT,		"NUMBER",	0,	"Network port (default port 70)"},
	{"timeout",		KEY_TIMEOUT,	"NUMBER",	0,	"Time in seconds before booting inactive client (default 10 seconds)"},
	{"workers",		KEY_WORKERS,	"NUMBER",	0,	"Number of worker processes (default 1 worker)"},
	{0}
};

// documentation string
static char argp_doc[] = "Server for the Gopher protocol.";

// argp globals (these must have these names)
const char* argp_program_version = "0.1";
const char* argp_program_bug_address = "<contact@sarahwatt.ca>";

//
static error_t argp_parse_options(int key, char* arg, struct argp_state* state)
{
	struct args_t* args = state->input;

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
// Worker and supervisor state definitions
// *********************************************************************

struct worker_t
{
	unsigned int number;
	pid_t pid;
	int pidfd;
};

struct supervisor_t
{
	struct worker_t* workers;
	unsigned int numWorkers;
	unsigned int activeWorkers;
	int sigfd;
	struct sepoll_t* loop;
};

// *********************************************************************
// Send a signal to all workers via pidfds, optionally closing the pidfd
// *********************************************************************
static void signal_all_workers(struct supervisor_t* supervisor, int sig, bool close_pidfd)
{
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		if (supervisor->workers[i].pidfd >= 0)
		{
			if (pidfd_send_signal(supervisor->workers[i].pidfd, sig, NULL, 0) < 0)
			{
				fprintf(stderr, "S - Error: Cannot send signal to child via pidfd: %m\n");
			}
			
			if (close_pidfd)
			{
				close(supervisor->workers[i].pidfd);
			}
		}
	}
}

// *********************************************************************
// Handle event on a signalfd
// *********************************************************************
static void sigfd_event(uint32_t events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct supervisor_t* supervisor = userdata1.ptr;
	
	while (1)
	{
		struct signalfd_siginfo siginfo;
		
		if (read(supervisor->sigfd, &siginfo, sizeof(struct signalfd_siginfo)) != sizeof(struct signalfd_siginfo))
		{
			if (errno == EAGAIN)
			{
				break;
			}
			else
			{
				fprintf(stderr, "S - Error: Cannot read from signalfd: %m\n");
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

// *********************************************************************
// Handle event on a pidfd
// *********************************************************************
static void pidfd_event(uint32_t events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct supervisor_t* supervisor = userdata1.ptr;
	struct worker_t* worker = userdata2.ptr;
	
	// Get the exit code and reap the child process
	siginfo_t siginfo;
	
	if (waitid(P_PIDFD, (id_t)worker->pidfd, &siginfo, WEXITED) < 0)
	{
		fprintf(stderr, "S - Worker PID %i exited but waitid failed: %m\n", worker->pid);
	}
	else
	{
		fprintf(stderr, "S - Worker PID %i exited with status %i\n", worker->pid, siginfo.si_status);
	}
	
	// Deal with the file descriptor
	sepoll_remove(supervisor->loop, worker->pidfd);
	close(worker->pidfd);
	
	worker->pidfd = -1;
	
	// Exit the event loop if there are no more active workers
	supervisor->activeWorkers--;
	
	if (supervisor->activeWorkers == 0)
	{
		sepoll_exit(supervisor->loop);
	}
}

// *********************************************************************
// Supervisor cleanup function for on_exit
// *********************************************************************
static void supervisor_cleanup(int code, void* arg)
{
	struct supervisor_t* supervisor = arg;
	
	if (supervisor->workers != NULL)
	{
		// On a successful exit the children should have exited already
		if (code == EXIT_FAILURE)
		{
			signal_all_workers(supervisor, SIGKILL, true);
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
// Main
// *********************************************************************
int main(int argc, char* argv[])
{
	// argp parser options
	struct argp argp_parser = {argp_options, argp_parse_options, 0, argp_doc};
	
	// Default argument values
	struct args_t args =
	{
		.directory = "./gopherroot",
		.hostname = "localhost",
		.indexfile = ".gophermap",
		.maxClients = 1000,
		.port = 70,
		.timeout = 10,
		.numWorkers = 1
	};

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
	// In the future these could potentially also come from config files
	struct server_params_t params =
	{
		.hostname = args.hostname,
		.directory = args.directory,
		.port = args.port,
		.maxClients = args.maxClients,
		.indexfile = args.indexfile,
		.timeout = args.timeout
	};
	
	// Where we're going we only need stderr
	int devnull = open("/dev/null", O_RDWR);
	
	if (devnull < 0)
	{
		fprintf(stderr, "S - Error: Cannot open /dev/null: %m\n");
		exit(EXIT_FAILURE);
	}
	
	if (dup2(devnull, STDIN_FILENO) < 0)
	{
		fprintf(stderr, "S - Error: Cannot dup2 /dev/null over stdin: %m\n");
		close(devnull);
		exit(EXIT_FAILURE);
	}
	
	if (dup2(devnull, STDOUT_FILENO) < 0)
	{
		fprintf(stderr, "S - Error: Cannot dup2 /dev/null over stdout: %m\n");
		close(devnull);
		exit(EXIT_FAILURE);
	}
	
	close(devnull);
	
	// Allocate and set up supervisor
	struct supervisor_t* supervisor = malloc(sizeof(struct supervisor_t));
	
	if (supervisor == NULL)
	{
		fprintf(stderr, "S - Error: Cannot allocate memory for supervisor: %m\n");
		exit(EXIT_FAILURE);
	}
	
	supervisor->numWorkers = args.numWorkers;
	supervisor->activeWorkers = 0;
	supervisor->sigfd = -1;
	supervisor->loop = NULL;
	
	// Allocate and set up workers
	supervisor->workers = calloc(supervisor->numWorkers, sizeof(struct worker_t));
	
	if (supervisor->workers == NULL)
	{
		fprintf(stderr, "S - Error: Cannot allocate memory for workers: %m\n");
		free(supervisor);
		exit(EXIT_FAILURE);
	}
	
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		supervisor->workers[i].pidfd = -1;
	}
	
	// Spawn worker processes
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		int pidfd;
		
		// This custom fork returns both a pid and a pidfd to the parent
		pid_t pid = sfork(&pidfd, 0);
	
		if (pid == 0) // Worker
		{
			// Close pidfds
			for (unsigned int j = 0; j < i; j++)
			{
				close(supervisor->workers[j].pidfd);
			}
			
			// No point keeping these around in the worker process
			free(supervisor->workers);
			free(supervisor);
			
			// This does not return
			server_process(&params);
		}
		else if (pid < 0) // Error
		{
			fprintf(stderr, "S - Error: Cannot fork worker process %u - %m\n", i);
		}
		else // Parent
		{
			fprintf(stderr, "S - Spawned worker process %u (PID %i)\n", i, pid);
			
			// Keep track of the forked worker
			supervisor->workers[i].number = i;
			supervisor->workers[i].pid = pid;
			supervisor->workers[i].pidfd = pidfd;
			
			supervisor->activeWorkers++;
		}
	}
	
	// Supervisor task begins here
	on_exit(supervisor_cleanup, supervisor);
	
	// Check number of workers that were successfully spawned
	if (supervisor->activeWorkers == 0)
	{
		fprintf(stderr, "S - Could not spawn any workers!\n");
		exit(EXIT_FAILURE);
	}
	else if (supervisor->activeWorkers < supervisor->numWorkers)
	{
		fprintf(stderr, "S - Could only spawn %u workers instead of the requested %u\n", supervisor->activeWorkers, supervisor->numWorkers);
	}
	else
	{
		fprintf(stderr, "S - All workers spawned\n");
	}

	// Set up signals and the signalfd
	sigset_t mask;
	
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
	{
		fprintf(stderr, "S - Error: Cannot block signals: %m\n");
		exit(EXIT_FAILURE);
	}
	
	supervisor->sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
	
	if (supervisor->sigfd < 0)
	{
		fprintf(stderr, "S - Error: Cannot open signalfd: %m\n");
		exit(EXIT_FAILURE);
	}
	
	// Create event loop with enough room for responses from each worker and the signalfd in one loop
	supervisor->loop = sepoll_create((int)supervisor->numWorkers + 1, 0);
	
	if (supervisor->loop == NULL)
	{
		fprintf(stderr, "S - Error: Cannot create event loop!\n");
		exit(EXIT_FAILURE);
	}
	
	sepoll_add(supervisor->loop, supervisor->sigfd, EPOLLIN | EPOLLET, sigfd_event, supervisor, NULL);
	
	for (unsigned int i = 0; i < supervisor->numWorkers; i++)
	{
		if (supervisor->workers[i].pidfd > -1)
		{
			sepoll_add(supervisor->loop, supervisor->workers[i].pidfd, EPOLLIN, pidfd_event, supervisor, &supervisor->workers[i]);
		}
	}
	
	// Event loop doesn't exit until all children exit
	sepoll_enter(supervisor->loop, -1, NULL, NULL);
	
	fprintf(stderr, "S - All workers exited\n");
	
	exit(EXIT_SUCCESS);
}
