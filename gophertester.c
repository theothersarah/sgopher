// Argument handling
#include <argp.h>

// Internet shit
#include <arpa/inet.h>
#include <netinet/in.h>

// errno
#include <errno.h>

// poll
#include <poll.h>

// sscanf, printf, fprintf
#include <stdio.h>

// malloc, free, on_exit, exit
#include <stdlib.h>

// strerror, strlen
#include <string.h>

// mmap, munmap
#include <sys/mman.h>

// socket, connect
#include <sys/socket.h>

// timerfd API
#include <sys/timerfd.h>

// writev
#include <sys/uio.h>

// wait
#include <sys/wait.h>

// fork, read, close
#include <unistd.h>

// *********************************************************************
// argp stuff for option parsing
// *********************************************************************

// argp globals (these must have these names)
const char* argp_program_version = "gophertester 0.1";
//const char* argp_program_bug_address = "";

// argp documentation string
static char argp_doc[] = "Benchmark tool for Gopher servers";

// Constants for arguments
enum arg_keys
{
	KEY_ADDRESS = 'a',
	KEY_DURATION = 'd',
	KEY_PORT = 'p',
	KEY_REQUEST = 'r',
	KEY_SIZE = 's',
	KEY_TIMEOUT = 't',
	KEY_WORKERS = 'w'
};

// argp options vector
static struct argp_option argp_options[] =
{
	{"address",		KEY_ADDRESS,	"STRING",		0,			"Address of echo server (default 127.0.0.1)"},
	{"duration",	KEY_DURATION,	"NUMBER",		0,			"Duration of test in seconds (default 60)"},
	{"port",		KEY_PORT,		"NUMBER",		0,			"Network port to use (default 8080)"},
	{"request",		KEY_REQUEST,	"STRING",		0,			"Request string (default /)"},
	{"size",		KEY_SIZE,		"NUMBER",		0,			"Expected size of response in bytes (default 0 - do not check size)"},
	{"timeout",		KEY_TIMEOUT,	"NUMBER",		0,			"Time to wait for socket state change before giving up in milliseconds (default 1000)"},
	{"workers",		KEY_WORKERS,	"NUMBER",		0,			"Number of worker processes (default 1)"},
	{0}
};

// Program arguments
struct arguments
{
	char* address;
	unsigned int duration;
	unsigned short port;
	char* request;
	unsigned int size;
	int timeout;
	unsigned int workers;
};

// Argp option parser
static error_t argp_parse_options(int key, char* arg, struct argp_state* state)
{
	struct arguments* args = state->input;

	switch (key)
	{
	case KEY_ADDRESS:
		args->address = arg;
		break;
	case KEY_DURATION:
		sscanf(arg, "%u", &args->duration);
		break;
	case KEY_PORT:
		sscanf(arg, "%hu", &args->port);
		break;
	case KEY_REQUEST:
		args->request = arg;
		break;
	case KEY_SIZE:
		sscanf(arg, "%u", &args->size);
		break;
	case KEY_TIMEOUT:
		sscanf(arg, "%i", &args->timeout);
		break;
	case KEY_WORKERS:
		sscanf(arg, "%u", &args->workers);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	
	return 0;
}

// *********************************************************************
// Shared memory allocation functions
//
// smalloc asks for a little extra memory so it can prepend a size,
// which is used by sfree when later being freed. scalloc is a wrapper
// around smalloc which is for allocating an array.
// *********************************************************************

void* smalloc(size_t size)
{
	if (size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t length = sizeof(size_t) + size;

	size_t* ptr = (size_t*)mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED)
	{
		return NULL;
	}

	*ptr = length;
	ptr++;

	return (void*)ptr;
}

void* scalloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t total = nmemb * size;
	
	if (total / nmemb != size)
	{
		errno = EOVERFLOW;
		return NULL;
	}
	
	return smalloc(total);
}

void sfree(void* ptr)
{
	if (ptr == NULL)
	{
		return;
	}

	size_t* base = (size_t*)ptr;
	base--;

	munmap((void*)base, *base);
}

// *********************************************************************
// Shared memory which is dynamically allocated and then freed at exit
// *********************************************************************

struct result
{
	pid_t pid;
	int status;
	long total;
	long successful;
	long timeout;
	long mismatch;
};

void cleanup(int code, void* arg)
{
	struct result* results = arg;
	
	sfree(results);
}

// *********************************************************************
// Task for worker processes
// *********************************************************************
int process(unsigned int id, struct arguments* args, void* rxBuffer, size_t rxBufferSize, struct result* results)
{
	// Initialize success counter in shared memory
	results[id].total = 0;
	results[id].successful = 0;
	results[id].timeout = 0;
	results[id].mismatch = 0;
	
	// Prepare request vector which concatenates CRLF onto the end of the supplied string
	char* requestEnd = "\r\n";
	
	struct iovec request[] =
	{
		{
			.iov_base = args->request,
			.iov_len = strlen(args->request)
		},
		{
			.iov_base = requestEnd,
			.iov_len = strlen(requestEnd)
		}
	};
	
	ssize_t requestSize = (ssize_t)(request[0].iov_len + request[1].iov_len);
	
	// Set up socket address
	struct sockaddr_in addr = 
	{
		.sin_family = AF_INET,
		.sin_port = htons(args->port),
		.sin_addr =
		{
			.s_addr = htonl(INADDR_ANY)
		}
	};
	
	inet_pton(AF_INET, args->address, &addr.sin_addr.s_addr);
	
	// Set up timerfd, which is used to figure out when the test needs to stop
	struct itimerspec timer =
	{
		.it_interval =
		{
			.tv_sec = args->duration
		},
		
		.it_value =
		{
			.tv_sec = args->duration
		}
	};
	
	int timerfd = timerfd_create(CLOCK_REALTIME, 0);
	
	if (timerfd < 0)
	{
		fprintf(stderr, "Error: Worker #%u cannot create timerfd: %s\n", id, strerror(errno));
		return -1;
	}
	
	if (timerfd_settime(timerfd, 0, &timer, NULL) < 0)
	{
		fprintf(stderr, "Error: Worker #%u cannot set timerfd time: %s\n", id, strerror(errno));
		return -1;
	}
	
	// Initialize poll list used to monitor the socket and timerfd
	struct pollfd poll_list[2];
	
	poll_list[0].fd = timerfd;
	poll_list[0].events = POLLIN;
	
	while (1)
	{
		// Score an attempt
		results[id].total++;
		
		// Accumulator for received file size
		ssize_t received = 0;
		
		// Open socket
		int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
		
		if (sockfd < 0)
		{
			fprintf(stderr, "Error: Worker #%u cannot open socket: %s\n", id, strerror(errno));
			return -1;
		}
		
		// Connect to the host
		if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
		{
			// The socket is nonblocking so it should return EINPROGRESS
			if (errno != EINPROGRESS)
			{
				fprintf(stderr, "Error: Worker #%u cannot connect to server: %s\n", id, strerror(errno));
				return -1;
			}
		}
		
		// Add socket to poll list
		poll_list[1].fd = sockfd;
		poll_list[1].events = POLLOUT;
		
		while (1)
		{
			// Wait on the socket and timer FDs
			int n = poll(poll_list, 2, args->timeout);
			
			if (n < 0)
			{
				fprintf(stderr, "Error: Worker #%u cannot poll FDs: %s\n", id, strerror(errno));
				return -1;
			}
			else if (n == 0)
			{
				// Report a timeout only the first time it happens and then score it
				if (results[id].timeout == 0)
				{
					fprintf(stderr, "Warning: Worker #%u timed out\n", id);
				}
				
				results[id].timeout++;
				
				break;
			}
			
			// If timerfd ticks, we're done after the next completion or timeout
			if (poll_list[0].revents & POLLIN)
			{
				close(timerfd);
				
				// This causes the timer FD entry in the poll list to be ignored by subsequent calls to poll
				poll_list[0].fd = -1;
			}
			
			// Check socket state. Only in or out should occur at the same time since the events bitmap is used as a state machine
			if (poll_list[1].revents & POLLOUT)
			{
				// Send the request message
				ssize_t n = writev(sockfd, request, 2);
				
				if (n < 0)
				{
					fprintf(stderr, "Error: Worker #%u cannot write to socket: %s\n", id, strerror(errno));
					return -1;
				}
				else if (n != requestSize)
				{
					// The request should be small enough to send the full string in one go so something is wrong if not
					fprintf(stderr, "Error: Worker #%u cannot write full request string\n", id);
					return -1;
				}
				
				// Now we want to listen for a reply
				poll_list[1].events = POLLIN;
			}
			else if (poll_list[1].revents & POLLIN)
			{
				// Read until it blocks
				while (1)
				{
					ssize_t n = read(sockfd, rxBuffer, rxBufferSize);
					
					if (n < 0)
					{
						// If it would block then that's okay, but halt on any other error
						if (errno == EAGAIN || errno == EWOULDBLOCK)
						{
							break;
						}
						else
						{
							fprintf(stderr, "Error: Worker #%u cannot read socket: %s\n", id, strerror(errno));
							return -1;
						}
					}
					else if (n == 0)
					{
						// End of file means we figure out what to score and then do a double-break to repeat everything all over again
						if (args->size > 0 && received != args->size)
						{
							// Report a size mismatch only the first time it happens and then score it
							if (results[id].mismatch == 0)
							{
								fprintf(stderr, "Warning: Worker #%u size mismatch\n", id);
							}
							
							results[id].mismatch++;
						}
						else
						{
							// Score a successful result
							results[id].successful++;
						}
						
						// This has no relevance to poll itself, it's just used as a convenient way to pass on that we need to do a second break in a moment
						poll_list[1].events = 0;
						
						break;
					}
					
					received += n;
				}
				
				// Here's the second break
				if (poll_list[1].events == 0)
				{
					break;
				}
			}
		}
		
		close(sockfd);
		
		// If the timerfd ticked, we're done
		if (poll_list[0].fd < 0)
		{
			break;
		}
	}
	
	return 0;
}

// *********************************************************************
// Main function
// *********************************************************************
int main(int argc, char* argv[])
{
	// argp parser options
	struct argp argp_parser = {argp_options, argp_parse_options, 0, argp_doc};
	
	// Default argument values
	struct arguments args;
	args.address = "127.0.0.1";
	args.duration = 60;
	args.port = 70;
	args.request = "/";
	args.size = 0;
	args.timeout = 1000;
	args.workers = 1;

	// Parse arguments
	argp_parse(&argp_parser, argc, argv, 0, 0, &args);

	// Report argument values
	fprintf(stderr, "Address: %s\n", args.address);
	fprintf(stderr, "Port: %hu\n", args.port);
	fprintf(stderr, "Duration: %i seconds\n", args.duration);
	fprintf(stderr, "Request: %s\n", args.request);
	fprintf(stderr, "Expected size: %i\n", args.size);
	fprintf(stderr, "Timeout: %i milliseconds\n", args.timeout);
	fprintf(stderr, "Workers: %i\n", args.workers);
	
	// Allocate shared memory
	struct result* results = (struct result*)scalloc(args.workers, sizeof(struct result));
	
	if (results == NULL)
	{
		fprintf(stderr, "Error: Cannot allocate shared memory for results: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	on_exit(cleanup, results);
	
	// Spawn worker processes
	for (unsigned int i = 0; i < args.workers; i++)
	{
		pid_t pid = fork();
	
		if (pid == 0) // Worker
		{
			size_t rxBufferSize = 1024*1024;
			void* rxBuffer = malloc(rxBufferSize);
			
			if (rxBuffer == NULL)
			{
				fprintf(stderr, "Error: Worker #%i cannot allocate receive buffer: %s\n", i, strerror(errno));
				exit(EXIT_FAILURE);
			}
			
			int retval = process(i, &args, rxBuffer, rxBufferSize, results);
			
			free(rxBuffer);
			
			if (retval < 0)
			{
				exit(EXIT_FAILURE);
			}
			else
			{
				exit(EXIT_SUCCESS);
			}
		}
		else if (pid < 0) // Error
		{
			fprintf(stderr, "Error: Cannot create worker process #%i: %s\n", i, strerror(errno));
			exit(EXIT_FAILURE);
		}
		else // Parent
		{
			// Store mapping between our internal id and the process id
			results[i].pid = pid;
			results[i].status = -1;
		}
	}

	fprintf(stderr, "All worker processes dispatched, waiting for results\n");
	
	// Wait for workers to exit
	pid_t pid;
	int status;
	
	while (pid = wait(&status), pid > 0)
	{
		// Search process table to find out which worker had just exited
		for (unsigned int i = 0; i < args.workers; i++)
		{
			if (results[i].pid == pid)
			{
				if (status != 0)
				{
					fprintf(stderr, "Warning: Worker process #%i (PID %i) exited with status %i\n", i, pid, status);
				}
				
				results[i].status = status;

				break;
			}
		}
	}
	
	// Sum the results from workers that successfully exited
	struct result final =
	{
		.total = 0,
		.successful = 0,
		.timeout = 0,
		.mismatch = 0
	};
	
	int count = 0;
	
	for (unsigned int i = 0; i < args.workers; i++)
	{
		if (results[i].status == 0)
		{
			final.total += results[i].total;
			final.successful += results[i].successful;
			final.timeout += results[i].timeout;
			final.mismatch += results[i].mismatch;

			count++;
		}
	}

	// Report count of successes
	fprintf(stderr, "%i process(es) exited successfully\n", count);
	
	if (count == 0)
	{
		fprintf(stderr, "Because no processes exited successfully, a result cannot be calculated\n");
		exit(EXIT_FAILURE);
	}

	// Do the final calculation and report the final results
	printf("Number of attempts: %li\n", final.total);
	printf("Rate of attempts: %li per second\n", final.total / args.duration);
	printf("Number of successful requests: %li\n", final.successful);
	printf("Rate of successful requests: %li per second\n", final.successful / args.duration);
	
	if (final.timeout > 0)
	{
		printf("Number of timeouts: %li\n", final.timeout);
	}
	
	if (final.mismatch > 0)
	{
		printf("Number of size mismatches: %li\n", final.mismatch);
	}
	
	exit(EXIT_SUCCESS);
}
