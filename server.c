// For some especially non-standard things: memmem, mempcpy, accept4, O_PATH
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

// fprintf, snprintf, dprintf
#include <stdio.h>

// malloc, free, on_exit, exit
#include <stdlib.h>

// strerror, memchr, memmem, stpcpy, mempcpy, strrchr
#include <string.h>

// pidfd_send_signal
#include <sys/pidfd.h>

// prctl
#include <sys/prctl.h>

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

// read, close, getpid, dup2, dup, fexecve, fchdir
#include <unistd.h>

// event loop functions
#include "sepoll.h"

// server entry function and parameters
#include "server.h"

// sfork
#include "sfork.h"

// *********************************************************************
// Constants
// *********************************************************************

// File descriptors needed for the server: 3 standard, 5 for server core functions, 1 for incoming user, 1 for dup in CGI process
#define FDS_SERVER (3 + 5 + 1 + 1)

// File descriptors needed per client
#define FDS_CLIENT 4

// Maximum incoming request size
// Equal to twice the 255 bytes mandated by the gopher protocol plus 2 for the CRLF
#define MAX_REQUEST_SIZE 2*255 + 2

// Maximum filename size for a request that has been processed into a null-terminated relative path
// -2 because CRLF is cut off and +3 to add ./ to the start, a null to the end, and possibly a trailing / for a directory
#define MAX_FILENAME_SIZE MAX_REQUEST_SIZE - 2 + 4

// Size of buffers for CGI environment variables
// Arbitrary but generous, and if it's exceeded the string is truncated safely
#define ENV_BUFFER_SIZE 1024

// Error messages plus a format string to make them into gopher menus
#define ERROR_FORMAT "3%s\r\n.\r\n"

#define ERROR_BAD "400 Bad Request"
#define ERROR_FORBIDDEN "403 Forbidden"
#define ERROR_NOTFOUND "404 Not Found"
#define ERROR_TIMEOUT "408 Request Timeout"
#define ERROR_INTERNAL "500 Internal Server Error"
#define ERROR_UNAVAILABLE "503 Service Unavailable"

// *********************************************************************
// Definitions
// *********************************************************************
struct client_t
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
	int dirfd;
	int pidfd;
	
	LIST_ENTRY(client_t) entry;
};

LIST_HEAD(client_list_t, client_t);

struct server_t
{
	// Configuration parameters
	struct server_params_t* params;
	
	// File descriptors
	int directory;
	int socket;
	int sigfd;
	int timerfd;

	// Event loop
	struct sepoll_t* loop;
	
	// Clients
	unsigned int numClients;
	struct client_list_t clients;
};

// *********************************************************************
// Disconnect a client from the server
// *********************************************************************
static void client_disconnect(struct server_t* server, struct client_t* client)
{
	// Deal with the open file, if any
	if (client->file >= 0)
	{
		close(client->file);
	}
	
	// Deal with the open directory, if any
	if (client->dirfd >= 0)
	{
		close(client->dirfd);
	}
	
	// Deal with the pidfd, if any
	if (client->pidfd >= 0)
	{
		sepoll_remove(server->loop, client->pidfd);
		close(client->pidfd);
	}
	
	// Deal with the socket
	sepoll_remove(server->loop, client->socket);
	close(client->socket);
	
	// Get rid of the list entry
	LIST_REMOVE(client, entry);
	free(client);
	
	// Update the client count
	server->numClients--;
}

// *********************************************************************
// Send a kill signal to a pidfd and disconnect the client if it fails
// *********************************************************************
static inline void pidfd_kill_client(struct server_t* server, struct client_t* client)
{
	if (pidfd_send_signal(client->pidfd, SIGKILL, NULL, 0) < 0)
	{
		// This shouldn't fail undless something is deeply wrong
		// In the event that it does, boot the client and pray for the best
		fprintf(stderr, "%i - Error: Cannot send kill signal via pidfd: %s\n", getpid(), strerror(errno));
		
		client_disconnect(server, client);
	}
}

// *********************************************************************
// Handle event on pidfd
// *********************************************************************
static void client_pidfd(unsigned int events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct server_t* server = userdata1.ptr;
	struct client_t* client = userdata2.ptr;
	
	// Now that the process has ended we can disconnect the client
	client_disconnect(server, client);
}

// *********************************************************************
// Handle event on a client socket
// *********************************************************************
static void client_socket(unsigned int events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct server_t* server = userdata1.ptr;
	struct client_t* client = userdata2.ptr;
	
	if (events & EPOLLIN)
	{
		// Read socket into client's buffer until it is full or the read would block
		do
		{
			ssize_t count = read(client->socket, client->buffer + client->count, MAX_REQUEST_SIZE - client->count);
			
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
					dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
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
			dprintf(client->socket, ERROR_FORMAT, ERROR_BAD);
			client_disconnect(server, client);
			return;
		}
		
		// Selector and query position and size within the client buffer
		// Selector will be at the beginning so it doesn't need a pointer
		size_t selectorSize;
		
		char* query;
		size_t querySize;
		
		// Search for a tab which indicates that the request contains a query
		char* tab = memchr(client->buffer, '\t', client->count);
		
		// Figure out the length of the provided selector and the query
		if (tab != NULL && tab < crlf)
		{
			selectorSize = (size_t)(tab - client->buffer);
			querySize = (size_t)(crlf - tab - 1);
		}
		else
		{
			selectorSize = (size_t)(crlf - client->buffer);
			querySize = 0;
		}
		
		// Query size could still have been zero even if there was a tab
		if (querySize > 0)
		{
			query = tab + 1;
		}
		else
		{
			query = NULL;
		}
		
		// Buffer for processed filename and pointer to last slash within it for determination of pathname and basename
		char filename[MAX_FILENAME_SIZE];
		
		char* filename_end = stpcpy(filename, ".");
		
		// Inspect provided path for leading periods to prevent use of relative paths and access to hidden files
		// While we're at it, let's remove redundant and trailing slashes as we copy it to the buffer
		if (selectorSize > 0)
		{
			char* str_pos = client->buffer;
			
			do
			{
				size_t str_len = selectorSize - (size_t)(str_pos - client->buffer);
				
				char* str_slash = memchr(str_pos, '/', str_len);
				
				size_t substr_len;
				
				if (str_slash == NULL)
				{
					substr_len = str_len;
				}
				else
				{
					substr_len = (size_t)(str_slash - str_pos);
				}
				
				if (substr_len > 0)
				{
					if (*str_pos == '.')
					{
						dprintf(client->socket, ERROR_FORMAT, ERROR_FORBIDDEN);
						client_disconnect(server, client);
						return;
					}
					
					filename_end = stpcpy(filename_end, "/");
					filename_end = mempcpy(filename_end, str_pos, substr_len);
				}
				
				str_pos = str_slash;
			}
			while (str_pos++ != NULL);
			
			*filename_end = '\0';
		}
		
		// Try to open the requested file
		client->file = openat(server->directory, filename, O_RDONLY | O_CLOEXEC);
		
		if (client->file < 0)
		{
			if (errno == ENOENT)
			{
				dprintf(client->socket, ERROR_FORMAT, ERROR_NOTFOUND);
			}
			else if (errno == EACCES)
			{
				dprintf(client->socket, ERROR_FORMAT, ERROR_FORBIDDEN);
			}
			else
			{
				fprintf(stderr, "%i - Error: Cannot open file %s: %s\n", getpid(), filename, strerror(errno));
				dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
			}
			
			client_disconnect(server, client);
			return;
		}
		
		// Get file stats
		struct stat statbuf;
		
		if (fstat(client->file, &statbuf) < 0)
		{
			fprintf(stderr, "%i - Error: Cannot fstat file %s: %s\n", getpid(), filename, strerror(errno));
			dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
			client_disconnect(server, client);
			return;
		}
		
		// Make sure it's a regular file or a directory
		if (S_ISREG(statbuf.st_mode))
		{
			// This space intentionally blank
			// At this point we could process the filename string to determine the containing directory path,
			// but since that's only relevant to CGI we wait until after the fork to do it to avoid doing it if we don't have to
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			// Keep track of the directory FD for CGI purposes
			client->dirfd = client->file;
			
			// Try to open an index file in the directory
			client->file = openat(client->dirfd, server->params->indexfile, O_RDONLY | O_CLOEXEC);
			
			if (client->file < 0)
			{
				if (errno == ENOENT)
				{
					dprintf(client->socket, ERROR_FORMAT, ERROR_NOTFOUND);
				}
				else if (errno == EACCES)
				{
					dprintf(client->socket, ERROR_FORMAT, ERROR_FORBIDDEN);
				}
				else
				{
					fprintf(stderr, "%i - Error: Cannot open file %s in directory %s: %s\n", getpid(), server->params->indexfile, filename, strerror(errno));
					dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
				}
				
				client_disconnect(server, client);
				return;
			}
			
			// Now we need the stats of the index file
			if (fstat(client->file, &statbuf) < 0)
			{
				fprintf(stderr, "%i - Error: Cannot fstat file %s in directory %s: %s\n", getpid(), server->params->indexfile, filename, strerror(errno));
				dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
				client_disconnect(server, client);
				return;
			}
			
			// Make sure it's a regular file
			if (!S_ISREG(statbuf.st_mode))
			{
				dprintf(client->socket, ERROR_FORMAT, ERROR_FORBIDDEN);
				client_disconnect(server, client);
				return;
			}
			
			// For the benefit of CGI programs, add a / to the end of the filename to indicate it was a directory
			filename_end = stpcpy(filename_end, "/");
		}
		else
		{
			dprintf(client->socket, ERROR_FORMAT, ERROR_FORBIDDEN);
			client_disconnect(server, client);
			return;
		}
		
		// If the file is world executable, fork off a process and try to execute it
		if (statbuf.st_mode & S_IXOTH)
		{
			// This custom fork returns both a pid and pidfd with one syscall
			pid_t pid = sfork(&client->pidfd, CLONE_CLEAR_SIGHAND);
			
			if (pid == 0)
			{
				// First argument for fexecve
				char* command;
				
				// If we don't already have a file descriptor for the containing directory, we need to figure one out from the filename
				if (client->dirfd < 0)
				{
					// Buffer for pathname
					char pathname[MAX_FILENAME_SIZE];
					
					// The filename should always contain a slash, but just in case...
					char* filename_slash = strrchr(filename, '/');
					
					if (filename_slash == NULL)
					{
						fprintf(stderr, "%i (CGI process) - Error: Cannot find slash in filename %s\n", getpid(), filename);
						dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
						exit(EXIT_FAILURE);
					}
					
					// Extract everything up to the last slash as a string and make it into a null-terminated string
					char* str_end = mempcpy(pathname, filename, (size_t)(filename_slash - filename));
					*str_end = '\0';
					
					// Try to open the path
					client->dirfd = openat(server->directory, pathname, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_PATH);
					
					if (client->dirfd < 0)
					{
						fprintf(stderr, "%i (CGI process) - Error: Cannot open %s: %s\n", getpid(), pathname, strerror(errno));
						dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
						exit(EXIT_FAILURE);
					}
					
					// Command is whatever is after the slash
					command = filename_slash + 1;
				}
				else
				{
					// Since we opened a directory to get here, that means we have opened a default file
					command = (char*)server->params->indexfile;
				}
				
				// Change working directory to the location of the executable file
				if (fchdir(client->dirfd) < 0)
				{
					fprintf(stderr, "%i (CGI process) - Error: Cannot fchdir: %s\n", getpid(), strerror(errno));
					dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
					exit(EXIT_FAILURE);
				}
				
				// Reset signal mask
				sigset_t mask;
				
				sigemptyset(&mask);
				
				if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0)
				{
					fprintf(stderr, "%i (CGI process) - Error: Cannot reset signal mask: %s\n", getpid(), strerror(errno));
					dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
					exit(EXIT_FAILURE);
				}
				
				// Replace the fork's stdout FD with the socket FD
				if (dup2(client->socket, STDOUT_FILENO) < 0)
				{
					fprintf(stderr, "%i (CGI process) - Error: Cannot dup2 socket over stdout: %s\n", getpid(), strerror(errno));
					dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
					exit(EXIT_FAILURE);
				}
				
				// Command line arguments
				char* const argv[] =
				{
					command,
					NULL
				};
				
				// Environment variables
				char env_selector[ENV_BUFFER_SIZE];
				snprintf(env_selector, ENV_BUFFER_SIZE, "SCRIPT_NAME=%s", filename + 1);
				
				char env_query[ENV_BUFFER_SIZE];
				snprintf(env_query, ENV_BUFFER_SIZE, "QUERY_STRING=%.*s", (int)querySize, query);
				
				char env_hostname[ENV_BUFFER_SIZE];
				snprintf(env_hostname, ENV_BUFFER_SIZE, "SERVER_NAME=%s", server->params->hostname);
				
				char env_port[ENV_BUFFER_SIZE];
				snprintf(env_port, ENV_BUFFER_SIZE, "SERVER_PORT=%hu", server->params->port);
				
				char env_address[ENV_BUFFER_SIZE];
				snprintf(env_address, ENV_BUFFER_SIZE, "REMOTE_ADDR=%s", client->address);
				
				char* envp[] =
				{
					env_selector,
					env_query,
					env_hostname,
					env_port,
					env_address,
					NULL
				};
				
				// dup here makes a copy of the file descriptor without the CLOEXEC flag, which prevents scripts from properly executing with fexecve
				fexecve(dup(client->file), argv, envp);
				
				// This is only reached if there was a problem with fexecve
				fprintf(stderr, "%i (CGI process) - Error: Cannot execute file %s: %s\n", getpid(), filename, strerror(errno));
				dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
				exit(EXIT_FAILURE);
			}
			else if (pid < 0)
			{
				fprintf(stderr, "%i - Error: Cannot fork CGI process: %s\n", getpid(), strerror(errno));
				dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
				client_disconnect(server, client);
				return;
			}
			
			// There's no need for the client's file to be open at this point
			close(client->file);
			client->file = -1;
			
			// Alter the events on the client socket to only handle errors
			sepoll_mod_events(server->loop, client->socket, EPOLLET);
			
			// Add the pidfd to the event loop
			sepoll_add(server->loop, client->pidfd, EPOLLIN, client_pidfd, server, client);
		}
		else
		{
			// Otherwise, transmit the file
			client->filesize = statbuf.st_size;
			sepoll_mod_events(server->loop, client->socket, EPOLLOUT | EPOLLET);
		}
		
		// If we opened a directory, it's no longer needed now
		if (client->dirfd >= 0)
		{
			close(client->dirfd);
			client->dirfd = -1;
		}
		
		// Update timestamp
		client->timestamp = time(NULL);
	}
	
	if (events & EPOLLOUT)
	{
		// Do sendfile until it would block or is complete
		do
		{
			ssize_t n = sendfile(client->socket, client->file, &client->sentsize, (size_t)(client->filesize - client->sentsize));
			
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				else if (errno != EPIPE)
				{
					// Don't bother reporting it if it's a broken pipe because that had nothing to do with us
					fprintf(stderr, "%i - Error: Problem sending file to client: %s\n", getpid(), strerror(errno));
					
					// Only send the error message if none of the file has been sent yet
					if (client->sentsize == 0)
					{
						dprintf(client->socket, ERROR_FORMAT, ERROR_INTERNAL);
					}
				}
				
				client_disconnect(server, client);
				return;
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
			pidfd_kill_client(server, client);
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
static void server_socket(unsigned int events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct server_t* server = userdata1.ptr;
	
	if (events & EPOLLIN)
	{
		// Accept incoming connections until it blocks. This is actually quite a bit faster than accepting one connection at a time before going back to do other things.
		while (1)
		{
			// Accept the next incoming connection
			struct sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);
			
			int fd = accept4(server->socket, (struct sockaddr*)&client_addr, &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
			
			if (fd < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					break;
				}
				else
				{
					fprintf(stderr, "%i - Error: Cannot accept incoming connection: %s\n", getpid(), strerror(errno));
					return;
				}
			}
			
			// Check if we have already hit the maximum number of clients
			if (server->numClients == server->params->maxClients)
			{
				// Server's full
				dprintf(fd, ERROR_FORMAT, ERROR_UNAVAILABLE);
				close(fd);
				continue;
			}
			
			// Allocate a new client data structure
			struct client_t* client = malloc(sizeof(struct client_t));
			
			if (client == NULL)
			{
				fprintf(stderr, "%i - Error: Cannot allocate memory for new client: %s\n", getpid(), strerror(errno));
				dprintf(fd, ERROR_FORMAT, ERROR_INTERNAL);
				close(fd);
				continue;
			}
			
			// Initialize the client, add their socket FD to the watch list, and add the client to the list
			client->socket = fd;
			client->timestamp = time(NULL);
			client->count = 0;
			client->file = -1;
			client->sentsize = 0;
			client->dirfd = -1;
			client->pidfd = -1;
			
			inet_ntop(AF_INET, &client_addr.sin_addr, client->address, INET_ADDRSTRLEN);
			
			sepoll_add(server->loop, fd, EPOLLIN | EPOLLET, client_socket, server, client);
			
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
static void server_signal(unsigned int events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct server_t* server = userdata1.ptr;
	
	while (1)
	{
		struct signalfd_siginfo siginfo;
		
		if (read(server->sigfd, &siginfo, sizeof(struct signalfd_siginfo)) != sizeof(struct signalfd_siginfo))
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}
			else
			{
				fprintf(stderr, "%i - Error: Cannot read from signalfd: %s\n", getpid(), strerror(errno));
				return;
			}
		}
		
		switch (siginfo.ssi_signo)
		{
		case SIGTERM:
			fprintf(stderr, "%i - Received SIGTERM\n", getpid());
			sepoll_exit(server->loop);
			break;
		}
	}
}

// *********************************************************************
// timerfd event handler
// *********************************************************************
static void server_timer(unsigned int events, union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct server_t* server = userdata1.ptr;
	
	// Have to read 8 bytes from the timer to reset it even if I don't care about the contents
	uint64_t buffer;
	
	if (read(server->timerfd, &buffer, sizeof(uint64_t)) != sizeof(uint64_t))
	{
		fprintf(stderr, "%i - Error: Cannot read from timerfd: %s\n", getpid(), strerror(errno));
	}
	
	// Check for connection timeout
	time_t currentTime = time(NULL);

	struct client_t* client = LIST_FIRST(&server->clients);
	
	while (client != NULL)
	{
		struct client_t* next = LIST_NEXT(client, entry);
		
		if (currentTime - client->timestamp >= server->params->timeout)
		{
			if (client->pidfd >= 0)
			{
				// The timestamp refers to when the CGI process was spawned,
				// so we need to spy on the TCP connection information to find out if it's really idle
				struct tcp_info tcp_info;
				socklen_t tcp_info_length = sizeof(struct tcp_info);
				
				int retval = getsockopt(client->socket, SOL_TCP, TCP_INFO, &tcp_info, &tcp_info_length);
				
				if (retval < 0)
				{
					fprintf(stderr, "%i - Error: Cannot get TCP information from socket: %s\n", getpid(), strerror(errno));
				}
				
				// Kill the child process if it hasn't used the socket for at least one timeout period
				if (retval < 0 || tcp_info.tcpi_last_data_sent >= server->params->timeout * 1000)
				{
					pidfd_kill_client(server, client);
				}
			}
			else
			{
				// Send timeout error if nothing has been sent yet
				if (client->sentsize == 0)
				{
					dprintf(client->socket, ERROR_FORMAT, ERROR_TIMEOUT);
				}
				
				client_disconnect(server, client);
			}
		}
		
		client = next;
	}
}

// *********************************************************************
// Set parent death signal and ignore signals that are counteractive to
// the program
// *********************************************************************
static int setupsignals()
{
	// We want to get SIGTERM if the parent dies
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot set signal to receive on parent death: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Ignore signals
	struct sigaction act =
	{
		.sa_handler = SIG_IGN
	};
	
	// We want to ignore SIGCHLD because we don't care about child exit codes and want it to be automatically reaped
	if (sigaction(SIGCHLD, &act, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot ignore  SIGCHLD: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// The client disconnecting during sendfile can cause SIGPIPE which kills the process, so we don't want that
	if (sigaction(SIGPIPE, &act, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot ignore  SIGPIPE: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	return 0;
}

// *********************************************************************
// Set the process open fd limit to what is needed given user limit
// *********************************************************************
static int increasefdlimit(unsigned int maxClients)
{
	struct rlimit limit;
	
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot get open file descriptor limit - %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	rlim_t cur = limit.rlim_cur;
	rlim_t max = limit.rlim_max;
	
	rlim_t inc = FDS_SERVER + maxClients * FDS_CLIENT;
	
	if (inc < cur)
	{
		return 0;
	}
	
	if (inc > max)
	{
		fprintf(stderr, "%i - Error: Process maximum FD limit too low to occomodate desired maximum number of clients\n", getpid());
		return -1;
	}
	
	limit.rlim_cur = inc;
	
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
	int dirfd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_PATH);
	
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
	
	// Make sure it's world readable
	if (!(statbuf.st_mode & S_IROTH))
	{
		fprintf(stderr, "%i - Error: Content path is not world readable\n", getpid());
		return -1;
	}
	
	// Make sure it's world executable
	if (!(statbuf.st_mode & S_IXOTH))
	{
		fprintf(stderr, "%i - Error: Content path is not world executable\n", getpid());
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
	// Block the signals we're interested in so that the signalfd can handle them
	sigset_t mask;
	
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot block signals: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Open the signalfd to handle the blocked signals
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
	// Open the timerfd
	int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	
	if (timerfd < 0)
	{
		fprintf(stderr, "%i - Error: Cannot open timerfd: %s\n", getpid(), strerror(errno));
		return -1;
	}
	
	// Set the timer interval to the client timeout period
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
// Server cleanup function for on_exit
// *********************************************************************
static void server_cleanup(int code, void* arg)
{
	struct server_t* server = arg;
	
	// Get rid of event loop
	if (server->loop != NULL)
	{
		sepoll_destroy(server->loop);
	}
	
	// Disconnect all clients
	// This is a slightly faster variant of calling client_disconnect
	// It frees all the resources and does none of the bookkeeping which no longer matters
	struct client_t* client = LIST_FIRST(&server->clients);
	
	while (client != NULL)
	{
		struct client_t* next = LIST_NEXT(client, entry);
		
		// Kill CGI process, if any
		if (client->pidfd >= 0)
		{
			pidfd_send_signal(client->pidfd, SIGKILL, NULL, 0);
			close(client->pidfd);
		}
		
		// Close the open file, if any
		if (client->file >= 0)
		{
			close(client->file);
		}
		
		// Close the open directory, if any
		if (client->dirfd >= 0)
		{
			close(client->dirfd);
		}
		
		// Close the socket
		close(client->socket);
		
		free(client);
		
		client = next;
	}
	
	// Close all the other FDs
	if (server->socket >= 0)
	{
		close(server->socket);
	}
	
	if (server->timerfd >= 0)
	{
		close(server->timerfd);
	}
	
	if (server->sigfd >= 0)
	{
		close(server->sigfd);
	}
	
	if (server->directory >= 0)
	{
		close(server->directory);
	}
	
	free(server);
}

// *********************************************************************
// Server setup and loop
// *********************************************************************
void server_process(struct server_params_t* params)
{
	// Set up signals
	if (setupsignals() < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Increase open file descriptor limit if needed
	if (increasefdlimit(params->maxClients) < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Allocate and set up the server state
	struct server_t* server = malloc(sizeof(struct server_t));
	
	if (server == NULL)
	{
		fprintf(stderr, "%i - Error: Could not allocate memory for server state: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	server->params = params;
	server->numClients = 0;
	
	LIST_INIT(&server->clients);
	
	server->directory = -1;
	server->sigfd = -1;
	server->timerfd = -1;
	server->socket = -1;
	
	server->loop = NULL;
	
	on_exit(server_cleanup, server);
	
	// Open content directory
	server->directory = open_dir(params->directory);
	
	if (server->directory < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Open signalfd
	server->sigfd = open_sigfd();
	
	if (server->sigfd < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Open timerfd
	server->timerfd = open_timerfd(params->timeout);
	
	if (server->timerfd < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Open socket
	server->socket = open_socket(params->port);
	
	if (server->socket < 0)
	{
		exit(EXIT_FAILURE);
	}
	
	// Set up epoll
	// Strictly speaking it doesn't need to be this big but it lets it handle an event from each client plus core things in one loop
	server->loop = sepoll_create((int)params->maxClients + 3);
	
	if (server->loop == NULL)
	{
		fprintf(stderr, "%i - Error: Cannot create event loop!\n", getpid());
		exit(EXIT_FAILURE);
	}
	
	sepoll_add(server->loop, server->sigfd, EPOLLIN | EPOLLET, server_signal, server, NULL);
	sepoll_add(server->loop, server->timerfd, EPOLLIN, server_timer, server, NULL);
	sepoll_add(server->loop, server->socket, EPOLLIN | EPOLLET, server_socket, server, NULL);
	
	// Enter event loop
	fprintf(stderr, "%i - Successfully started\n", getpid());
	
	sepoll_enter(server->loop);
	
	fprintf(stderr, "%i - Exiting\n", getpid());
	
	exit(EXIT_SUCCESS);
}
