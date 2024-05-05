// For non-standard things
#define _GNU_SOURCE

// All the internet shit
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

// errno
#include <errno.h>

// open, openat
#include <fcntl.h>

// sigaction, sigemptyset, sigaddset, sigprocmask
#include <signal.h>

// fprintf, snprintf
#include <stdio.h>

// malloc, free
#include <stdlib.h>

// strerror, memcpy, memchr, memmem
#include <string.h>

// pidfd_send_signal
#include <sys/pidfd.h>

// Linked list macros
#include <sys/queue.h>

// getrlimit, setrlimit
#include <sys/resource.h>

// sendfile
#include <sys/sendfile.h>

// signalfd
#include <sys/signalfd.h>

// socket, setsockopt, bind, listen, accept4, getsockopt
#include <sys/socket.h>

// fstat
#include <sys/stat.h>

// timerfd_create, timerfd_settime
#include <sys/timerfd.h>

// time
#include <time.h>

// read, write, close, getpid, dup2, dup, fexecve
#include <unistd.h>

//
#include "sfork.h"
#include "sepoll.h"
#include "server.h"

// *********************************************************************
// Constants
// *********************************************************************

// Twice the 255 bytes mandated by the gopher protocol plus 2 for the CRLF
#define MAX_REQUEST_SIZE 2*255 + 2

// Error messages formatted as gopher menus
#define ERROR_BAD "3400 Bad Request\r\n.\r\n"
#define ERROR_FORBIDDEN "3403 Forbidden\r\n.\r\n"
#define ERROR_NOTFOUND "3404 Not Found\r\n.\r\n"
#define ERROR_TIMEOUT "3408 Request Timeout\r\n.\r\n"
#define ERROR_INTERNAL "3500 Internal Server Error\r\n.\r\n"
#define ERROR_UNAVAILABLE "3503 Service Unavailable\r\n.\r\n"

// *********************************************************************
// Definitions
// *********************************************************************
struct client_state
{
	// Client session information
	int socket;
	char address[INET_ADDRSTRLEN];
	time_t timestamp;
	
	// Incoming request buffer
	size_t count;
	char buffer[MAX_REQUEST_SIZE];
	
	// File being transmitted
	int file;
	off_t filesize;
	off_t sentsize;
	
	// CGI
	int pidfd;
	
	LIST_ENTRY(client_state) entry;
};

LIST_HEAD(client_list, client_state);

struct server_state
{
	// Configuration parameters
	struct server_params* params;
	
	// File descriptors
	int directory;
	int socket;
	int sigfd;
	int timerfd;

	// Event loop
	struct sepoll_loop* loop;
	
	// Clients
	unsigned int numClients;
	struct client_list clients;
};

// *********************************************************************
// Disconnect a client from the server
// *********************************************************************
static void client_disconnect(struct server_state* server, struct client_state* client)
{
	// Deal with the open file, if any
	if (client->file >= 0)
	{
		sepoll_queue_close(server->loop, client->file);
	}
	
	// Deal with the socket
	sepoll_remove(server->loop, client->socket, 1);
	
	// Get rid of the list entry
	LIST_REMOVE(client, entry);
	free(client);
	
	// Update the client count
	server->numClients--;
}

// *********************************************************************
// Send a kill signal to a pidfd
// *********************************************************************
static inline void pidfd_kill(int pidfd)
{
	if (pidfd_send_signal(pidfd, SIGKILL, NULL, 0) < 0)
	{
		fprintf(stderr, "%i - Warning: Cannot send kill signal via pidfd: %s\n", getpid(), strerror(errno));
	}
}

// *********************************************************************
// Handle event on pidfd
// *********************************************************************
static void client_pidfd(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	struct client_state* client = userdata2;
	
	sepoll_remove(server->loop, fd, 1);
	
	client_disconnect(server, client);
}

// *********************************************************************
// Handle event on a client socket
// *********************************************************************
static void client_socket(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	struct client_state* client = userdata2;
	
	if (events & EPOLLIN)
	{
		// Read until buffer is full or read would block
		do
		{
			ssize_t count = read(fd, client->buffer + client->count, MAX_REQUEST_SIZE - client->count);
			
			if (count < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				else if (errno == ECONNRESET)
				{
					client_disconnect(server, client);
					return;
				}
				else
				{
					fprintf(stderr, "%i - Error: Cannot read from client: %s\n", getpid(), strerror(errno));
					write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					client_disconnect(server, client);
					return;
				}
			}
			else if (count == 0)
			{
				client_disconnect(server, client);
				return;
			}
			
			client->count += (size_t)count;
		}
		while (client->count < MAX_REQUEST_SIZE);
		
		// Search for crlf sequence
		char* crlf = memmem(client->buffer, client->count, "\r\n", 2);
		
		// No patience if a valid request didn't arrive yet after we did 2+ reads from the socket already
		if (crlf == NULL)
		{
			write(fd, ERROR_BAD, sizeof(ERROR_BAD) - 1);
			client_disconnect(server, client);
			return;
		}
		
		// Buffer for filename (size -2 because CRLF is cut off and +3 to add ./ to the start and null to the end)
		char filenameBuffer[MAX_REQUEST_SIZE - 2 + 3];
		size_t filenameSize;
		char* filename;
		
		// Buffer for query (size -2 because CRLF is cut off and +1 to add null to the end)
		char queryBuffer[MAX_REQUEST_SIZE - 2 + 1];
		size_t querySize = 0;
		char* query = NULL;
		
		// Search for a tab which indicates the request contains a query
		char* tab = memchr(client->buffer, '\t', client->count);
		
		// Figure out the length of the provided selector and the query
		if (tab != NULL && tab < crlf)
		{
			filenameSize = (size_t)(tab - client->buffer);
			querySize = (size_t)(crlf - tab - 1);
		}
		else
		{
			filenameSize = (size_t)(crlf - client->buffer);
		}
		
		// Figure out the filename
		if (filenameSize == 0)
		{
			filename = server->params->indexfile;
		}
		else
		{
			// Inspect provided path for leading periods to prevent use of relative paths and access to hidden files
			char* str_curr = client->buffer;
			
			do
			{
				size_t str_size = (size_t)(client->buffer + filenameSize - str_curr);
				
				if (str_size == 0)
				{
					break;
				}
				
				if (*str_curr == '.')
				{
					write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
					client_disconnect(server, client);
					return;
				}
				
				str_curr = memchr(str_curr, '/', str_size);
			}
			while (str_curr++ != NULL);
			
			// Ensure that the path is relative to the document root
			// It's okay if it already starts with a / because consecutive slashes are ignored
			filenameBuffer[0] = '.';
			filenameBuffer[1] = '/';
			memcpy(filenameBuffer + 2, client->buffer, filenameSize);
			filenameBuffer[filenameSize + 2] = '\0';
			
			filename = filenameBuffer;
		}
		
		// Fill the query buffer
		if (querySize > 0)
		{
			memcpy(queryBuffer, tab + 1, querySize);
			queryBuffer[querySize] = '\0';
			query = queryBuffer;
		}
		
		// Try to open the requested file
		client->file = openat(server->directory, filename, O_RDONLY | O_CLOEXEC);
		
		if (client->file < 0)
		{
			write(fd, ERROR_NOTFOUND, sizeof(ERROR_NOTFOUND) - 1);
			client_disconnect(server, client);
			return;
		}
		
		// Now let's figure out what to do with the file
		struct stat statbuf;
		
		while (1)
		{
			// Get file information
			if (fstat(client->file, &statbuf) < 0)
			{
				fprintf(stderr, "%i - Error: Cannot get file information: %s\n", getpid(), strerror(errno));
				write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
				client_disconnect(server, client);
				return;
			}
			
			// Check if world readable
			if (!(statbuf.st_mode & S_IROTH))
			{
				write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
				client_disconnect(server, client);
				return;
			}
			
			// Check if regular file
			if (S_ISREG(statbuf.st_mode))
			{
				break;
			}
			else
			{
				// If it's a directory, try to open up an index file within it
				if (S_ISDIR(statbuf.st_mode))
				{
					int indexfd = openat(client->file, server->params->indexfile, O_RDONLY | O_CLOEXEC);
					
					if (indexfd < 0)
					{
						write(fd, ERROR_NOTFOUND, sizeof(ERROR_NOTFOUND) - 1);
						client_disconnect(server, client);
						return;
					}
					
					// An index file was found so let's do the whole inspection dance with it again
					// This can result in an infinite loop if an index file is a symlink to its own directory
					// But if you do that you kind of deserve what you get I think
					sepoll_queue_close(server->loop, client->file);
					client->file = indexfd;
					
					continue;
				}
				else
				{
					write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
					client_disconnect(server, client);
					return;
				}
			}
		}
		
		// If the file is world executable, fork off a process and try to execute it
		if (statbuf.st_mode & S_IXOTH)
		{
			// This custom fork returns both a pid and pidfd with one syscall
			pid_t pid = sfork(&client->pidfd);
			
			if (pid == 0)
			{
				// Replace the fork's stdout FD with the socket FD
				if (dup2(fd, STDOUT_FILENO) < 0)
				{
					fprintf(stderr, "%i (CGI process) - Error: Cannot dup2 socket FD over stdout: %s\n", getpid(), strerror(errno));
					write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					exit(EXIT_FAILURE);
				}
				
				// Command line arguments
				char* argv[] =
				{
					filename,
					query,
					NULL
				};
				
				// Environment variables
				char selector[1024];
				snprintf(selector, 1024, "SELECTOR=%.*s", (int)filenameSize, client->buffer);
				
				char hostname[1024];
				snprintf(hostname, 1024, "HOSTNAME=%s", server->params->hostname);
				
				char port[1024];
				snprintf(port, 1024, "PORT=%hu", server->params->port);
				
				char* envp[] =
				{
					selector,
					hostname,
					port,
					NULL
				};
				
				// dup makes a copy of the file descriptor without the CLOEXEC flag, which prevents scripts from properly executing with fexecve
				fexecve(dup(client->file), argv, envp);

				// This is only reached if there was a problem with fexecve
				fprintf(stderr, "%i (CGI process) - Error: Cannot execute file %s: %s\n", getpid(), filename, strerror(errno));
				write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
				exit(EXIT_FAILURE);
			}
			else if (pid < 0)
			{
				fprintf(stderr, "%i - Error: Cannot fork CGI process: %s\n", getpid(), strerror(errno));
				write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
				client_disconnect(server, client);
				return;
			}
			
			// We don't need the file anymore since it should be off happily executing in the forked process
			sepoll_queue_close(server->loop, client->file);
			client->file = -1;
			
			// Alter the events on the client socket to only handle errors
			sepoll_mod_events(server->loop, fd, 0);
			
			// Add the pidfd to the event loop
			sepoll_add(server->loop, client->pidfd, EPOLLIN, client_pidfd, server, client);
		}
		else
		{
			// Otherwise, transmit the file
			client->filesize = statbuf.st_size;
			sepoll_mod_events(server->loop, fd, EPOLLOUT | EPOLLET);
		}
		
		// Update timestamp
		client->timestamp = time(NULL);
	}
	
	if (events & EPOLLOUT)
	{
		// Do sendfile until it would block or is complete
		do
		{
			ssize_t n = sendfile(fd, client->file, &client->sentsize, (size_t)(client->filesize - client->sentsize));
			
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				else
				{
					fprintf(stderr, "%i - Error: Problem sending file to client: %s\n", getpid(), strerror(errno));
					
					// Only send the error message if none of the file has been sent yet
					if (client->sentsize == 0)
					{
						write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					}
					
					client_disconnect(server, client);
					return;
				}
			}
		}
		while (client->sentsize < client->filesize);
		
		// See if transfer has not yet finished
		if (client->sentsize < client->filesize)
		{
			client->timestamp = time(NULL);
		}
		else
		{
			client_disconnect(server, client);
			return;
		}
	}
	
	if (events & EPOLLERR || events & EPOLLHUP)
	{
		if (client->pidfd >= 0)
		{
			pidfd_kill(client->pidfd);
		}
		else
		{
			client_disconnect(server, client);
		}
	}
}

// *********************************************************************
// Handle event on the listening socket
// *********************************************************************
static void server_socket(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	
	if (events & EPOLLIN)
	{
		// Accept incoming connections until it blocks. This is actually quite a bit faster than accepting one connection at a time before going back to do other things.
		while (1)
		{
			// Accept the next incoming connection
			struct sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			
			int client_fd = accept4(fd, (struct sockaddr*)&client_addr, &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
			
			if (client_fd < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				else
				{
					fprintf(stderr, "%i - Warning: Cannot accept incoming connection: %s\n", getpid(), strerror(errno));
					return;
				}
			}
			
			// Check if we have already hit the maximum number of clients
			if (server->numClients == server->params->maxClients)
			{
				// Server's full
				write(fd, ERROR_UNAVAILABLE, sizeof(ERROR_UNAVAILABLE) - 1);
				sepoll_queue_close(server->loop, client_fd);
				continue;
			}
			
			// Allocate a new client data structure
			struct client_state* client = malloc(sizeof(struct client_state));
			
			if (client == NULL)
			{
					write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					sepoll_queue_close(server->loop, client_fd);
					continue;
			}
			
			// Initialize the client, add their socket FD to the watch list, and add the client to the list
			client->socket = client_fd;
			client->timestamp = time(NULL);
			client->count = 0;
			client->file = -1;
			client->sentsize = 0;
			client->pidfd = -1;
			
			inet_ntop(AF_INET, &client_addr.sin_addr, client->address, INET_ADDRSTRLEN);
			
			sepoll_add(server->loop, client_fd, EPOLLIN | EPOLLET, client_socket, server, client);
			
			LIST_INSERT_HEAD(&server->clients, client, entry);
			
			server->numClients++;
		}
	}
	
	if (events & EPOLLERR)
	{
		fprintf(stderr, "%i - Error reported by listening socket\n", getpid());
	}

	if (events & EPOLLHUP)
	{
		fprintf(stderr, "%i - Hangup reported by listening socket\n", getpid());
	}
}

// *********************************************************************
// signalfd event handler
// *********************************************************************
static void server_signal(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	
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
				fprintf(stderr, "%i - Warning: Cannot read from signalfd: %s\n", getpid(), strerror(errno));
				return;
			}
		}
		
		switch (siginfo.ssi_signo)
		{
		case SIGTERM:
			sepoll_exit(server->loop);
			break;
		}
	}
}

// *********************************************************************
// timerfd event handler
// *********************************************************************
static void server_timer(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	
	// Have to read 8 bytes from the timer even if I don't care about it
	uint64_t buffer;
	
	if (read(fd, &buffer, sizeof(uint64_t)) != sizeof(uint64_t))
	{
		fprintf(stderr, "%i - Warning: Cannot read from timerfd: %s\n", getpid(), strerror(errno));
	}
	
	// Check for connection timeout
	time_t currentTime = time(NULL);

	struct client_state* client = LIST_FIRST(&server->clients);
	
	while (client != NULL)
	{
		struct client_state* next = LIST_NEXT(client, entry);
		
		if (client->pidfd >= 0)
		{
			struct tcp_info tcp_info;
			socklen_t tcp_info_length = sizeof(struct tcp_info);
			
			int retval = getsockopt(client->socket, SOL_TCP, TCP_INFO, &tcp_info, &tcp_info_length);
			
			if (retval < 0)
			{
				fprintf(stderr, "%i - Warning: Cannot get TCP information from socket: %s\n", getpid(), strerror(errno));
			}
			
			// Kill the child process if it hasn't used the socket for at least one timeout period
			if (retval < 0 || tcp_info.tcpi_last_data_sent >= server->params->timeout * 1000)
			{
				pidfd_kill(client->pidfd);
			}
		}
		else if (currentTime - client->timestamp >= server->params->timeout)
		{
			// Send timeout error if nothing has been sent yet
			if (client->sentsize == 0)
			{
				write(client->socket, ERROR_TIMEOUT, sizeof(ERROR_TIMEOUT) - 1);
			}
			
			client_disconnect(server, client);
		}
		
		client = next;
	}
}

// *********************************************************************
// Ignore signals that are counteractive to the program
// *********************************************************************
static int ignoresignals()
{
	struct sigaction act =
	{
		.sa_handler = SIG_IGN,
		
	};
	
	// We want to ignore SIGCHLD because we don't care about child exit codes and want it to be automatically reaped
	if (sigaction(SIGCHLD, &act, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot ignore  SIGCHLD: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// The client disconnecting during sendfile can cause SIGPIPE which kills the process, so we don't want that
	if (sigaction(SIGCHLD, &act, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot ignore  SIGPIPE: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	return 0;
}

// *********************************************************************
// Set the process open fd limit to the maximum allowed
// *********************************************************************
static int maxfdlimit()
{
	struct rlimit limit;
	
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot get open file descriptor limit - %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	limit.rlim_cur = limit.rlim_max;
	
	if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot set open file descriptor limit - %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	return 0;
}

// *********************************************************************
// Open the content directory
// *********************************************************************
static int open_dir(const char* directory)
{
	// Open content directory
	int dirfd = open(directory, O_RDONLY | O_CLOEXEC);
	
	if (dirfd < 0)
	{
		fprintf(stderr, "%i - Error: Cannot open content directory: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Get information for content directory
	struct stat statbuf;
	
	if (fstat(dirfd, &statbuf) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot get information about content directory: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Make sure it's actually a directory
	if (!S_ISDIR(statbuf.st_mode))
	{
		fprintf(stderr, "%i - Error: Content path is not a directory\n", getpid());
		return -1;
	}
	
	// Make sure it's world readable
	if (!(statbuf.st_mode & S_IROTH))
	{
		fprintf(stderr, "%i - Error: Content path is not world readable\n", getpid());
		return -1;
	}
	
	return dirfd;
}

// *********************************************************************
// Open the listening socket
// *********************************************************************
static int open_socket(unsigned short port)
{
	// Create socket
	int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
	
	if (sockfd < 0)
	{
		fprintf(stderr, "%i - Error: Cannot create socket: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Allow for address reuse
	int optval = 1;
	
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot enable address reuse on socket: %s\n", getpid(), strerror(errno));
		close(sockfd);
		return -1;
	}
	
	// Allow for port reuse
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot enable port reuse on socket: %s\n", getpid(), strerror(errno));
		close(sockfd);
		return -1;
	}
	
	// Bind address to socket
	struct sockaddr_in addr = 
	{
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr =
		{
			.s_addr = htonl(INADDR_ANY)
		}
	};
	
	if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot bind address to socket: %s\n", getpid(), strerror(errno));
		close(sockfd);
		return -1;
	}
	
	// Set up socket to listen
	if (listen(sockfd, 256) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot listen on socket: %s\n", getpid(), strerror(errno));
		close(sockfd);
		return -1;
	}
	
	return sockfd;
}

// *********************************************************************
// Open a signalfd
// *********************************************************************
static int open_sigfd()
{
	sigset_t mask;
	
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot block signals: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	
	if (sigfd < 0)
	{
		fprintf(stderr, "%i - Error: Cannot open signalfd: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	return sigfd;
}

// *********************************************************************
// Open a timerfd
// *********************************************************************
static int open_timerfd(time_t interval)
{
	int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	
	if (timerfd < 0)
	{
		fprintf(stderr, "%i - Error: Cannot open timerfd: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	struct itimerspec timer =
	{
		.it_interval =
		{
			.tv_sec = interval
		},
		
		.it_value =
		{
			.tv_sec = interval
		}
	};
	
	if (timerfd_settime(timerfd, 0, &timer, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot set timerfd: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	return timerfd;
}

// *********************************************************************
// Server setup and loop
// *********************************************************************
int doserver(struct server_params* params)
{
	// Ignore signals that we really don't want
	if (ignoresignals() < 0)
	{
		return -1;
	}
	
	// Max out open file descriptor limit
	if (maxfdlimit() < 0)
	{
		return -1;
	}
	
	// Set up the server state
	struct server_state server;
	
	server.params = params;
	server.numClients = 0;
	
	// Open content directory
	server.directory = open_dir(params->directory);
	
	if (server.directory < 0)
	{
		return -1;
	}
	
	// Open signalfd
	server.sigfd = open_sigfd();
	
	if (server.sigfd < 0)
	{
		return -1;
	}
	
	// Open timerfd
	server.timerfd = open_timerfd(params->timeout);
	
	if (server.timerfd < 0)
	{
		return -1;
	}
	
	// Open socket
	server.socket = open_socket(params->port);
	
	if (server.socket < 0)
	{
		return -1;
	}
	
	// Set up epoll
	server.loop = sepoll_create((int)params->maxClients + 3);
	sepoll_add(server.loop, server.sigfd, EPOLLIN | EPOLLET, server_signal, &server, NULL);
	sepoll_add(server.loop, server.timerfd, EPOLLIN, server_timer, &server, NULL);
	sepoll_add(server.loop, server.socket, EPOLLIN | EPOLLET, server_socket, &server, NULL);
	
	// Initialize client list
	LIST_INIT(&server.clients);
	
	// Enter event loop
	fprintf(stderr, "%i - Successfully started\n", getpid());
	sepoll_enter(server.loop);
	
	// Get rid of event loop
	sepoll_destroy(server.loop);
	
	// Disconnect all clients
	struct client_state* client = LIST_FIRST(&server.clients);
	
	while (client != NULL)
	{
		struct client_state* next = LIST_NEXT(client, entry);
		
		//
		if (client->pidfd >= 0)
		{
			pidfd_kill(client->pidfd);
			close(client->pidfd);
		}
		
		// Deal with the open file, if any
		if (client->file >= 0)
		{
			close(client->file);
		}
		
		// Deal with the socket
		close(client->socket);
		
		//
		free(client);
		
		client = next;
	}
	
	// Close all the other FDs
	close(server.socket);
	close(server.timerfd);
	close(server.sigfd);
	close(server.directory);
	
	return 0;
}
