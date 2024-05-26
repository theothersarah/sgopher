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

// strerror, memchr, memmem, strcpy, strncat, strrchr
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

// read, write, close, getpid, dup2, dup, fexecve, fchdir
#include <unistd.h>

// My stuff
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
	int dirfd;
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
		close(client->file);
	}
	
	// Deal with the open directory, if any
	if (client->dirfd >= 0)
	{
		close(client->dirfd);
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
// Send a kill signal to a pidfd
// *********************************************************************
static inline void pidfd_kill(int pidfd)
{
	if (pidfd_send_signal(pidfd, SIGKILL, NULL, 0) < 0)
	{
		fprintf(stderr, "%i - Error: Cannot send kill signal via pidfd: %s\n", getpid(), strerror(errno));
	}
}

// *********************************************************************
// Handle event on pidfd
// *********************************************************************
static void client_pidfd(int fd, unsigned int events, void* userdata1, void* userdata2)
{
	struct server_state* server = userdata1;
	struct client_state* client = userdata2;
	
	// Now that the process has ended we can boot the client
	client_disconnect(server, client);
	
	// Now we're done with the pidfd
	sepoll_remove(server->loop, fd);
	close(fd);
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
		
		// Selector and query position and size within the client buffer
		char* selector;
		size_t selectorSize;
		
		char* query;
		size_t querySize;
		
		// Buffer for filename (size -2 because CRLF is cut off and +3 to add ./ to the start and null to the end)
		char filename[MAX_REQUEST_SIZE - 2 + 3];
		
		// Search for a tab which indicates the request contains a query
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
		
		// Figure out the filename from the selector
		if (selectorSize == 0)
		{
			selector = NULL;
			
			strcpy(filename, "./");
		}
		else
		{
			selector = client->buffer;
			
			// Inspect provided path for leading periods to prevent use of relative paths and access to hidden files
			char* str_curr = client->buffer;
			
			do
			{
				size_t str_size = (size_t)(client->buffer + selectorSize - str_curr);
				
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
			strcpy(filename, "./");
			strncat(filename, selector, selectorSize);
		}
		
		// Try to open the requested file
		client->file = openat(server->directory, filename, O_RDONLY | O_CLOEXEC);
		
		if (client->file < 0)
		{
			write(fd, ERROR_NOTFOUND, sizeof(ERROR_NOTFOUND) - 1);
			client_disconnect(server, client);
			return;
		}
		
		// Get file information
		struct stat statbuf;
		
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
		
		// Make sure it's a regular file, or a directory that is world executable
		if (S_ISREG(statbuf.st_mode))
		{
			// This space intentionally blank
			// At this point we could process the filename string to determine the containing directory path,
			// but since that's only relevant to CGI we wait until after the fork to do it to avoid doing it if we don't have to
		}
		else if (S_ISDIR(statbuf.st_mode) && statbuf.st_mode & S_IXOTH)
		{
			// Keep track of the directory FD for CGI purposes
			client->dirfd = client->file;
			
			// Try to open an index file in the directory
			client->file = openat(client->file, server->params->indexfile, O_RDONLY | O_CLOEXEC);
			
			if (client->file < 0)
			{
				write(fd, ERROR_NOTFOUND, sizeof(ERROR_NOTFOUND) - 1);
				client_disconnect(server, client);
				return;
			}
			
			// Now we need the stats of the index file
			if (fstat(client->file, &statbuf) < 0)
			{
				fprintf(stderr, "%i - Error: Cannot get file information: %s\n", getpid(), strerror(errno));
				write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
				client_disconnect(server, client);
				return;
			}
			
			// If it isn't world readable or a regular file, get out of here
			if (!(statbuf.st_mode & S_IROTH) || !S_ISREG(statbuf.st_mode))
			{
				write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
				client_disconnect(server, client);
				return;
			}
		}
		else
		{
			write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
			client_disconnect(server, client);
			return;
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
				
				// If we don't already have a file descriptor for the containing directory, we need to figure one out from the filename
				if (client->dirfd < 0)
				{
					// It shouldn't be possible for it to not find a slash in the filename given that we added one
					char* slash = strrchr(filename, '/');
					
					// Extract everything up to the last slash as a string
					char path[1024];
					path[0] = '\0';
					strncat(path, filename, (size_t)(slash - filename));
					
					// Try to open the path
					client->dirfd = openat(server->directory, path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_PATH);
					
					if (client->dirfd < 0)
					{
						fprintf(stderr, "%i (CGI process) - Error: Cannot open executable file directory: %s\n", getpid(), strerror(errno));
						write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
						exit(EXIT_FAILURE);
					}
					
					// Try to get stats of the directory
					if (fstat(client->dirfd, &statbuf) < 0)
					{
						fprintf(stderr, "%i (CGI process) - Error: Cannot get file information: %s\n", getpid(), strerror(errno));
						write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
						exit(EXIT_FAILURE);
					}
					
					// If it isn't world readable or a world executable we're done
					if (!(statbuf.st_mode & S_IROTH) || !(statbuf.st_mode & S_IXOTH))
					{
						write(fd, ERROR_FORBIDDEN, sizeof(ERROR_FORBIDDEN) - 1);
						exit(EXIT_FAILURE);
					}
				}
				
				// Change working directory to the location of the executable file
				if (fchdir(client->dirfd) < 0)
				{
					fprintf(stderr, "%i (CGI process) - Error: Cannot fchdir to executable file directory: %s\n", getpid(), strerror(errno));
					write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					exit(EXIT_FAILURE);
				}
				
				// Command line arguments
				char* argv[] =
				{
					"cgi worker",
					NULL
				};
				
				// Environment variables
				// 1024 bytes should be more than generous and exceeding it will just cut it off
				char env_selector[1024];
				snprintf(env_selector, 1024, "SCRIPT_NAME=%.*s", (int)selectorSize, selector);
				
				char env_query[1024];
				snprintf(env_query, 1024, "QUERY_STRING=%.*s", (int)querySize, query);
				
				char env_hostname[1024];
				snprintf(env_hostname, 1024, "SERVER_NAME=%s", server->params->hostname);
				
				char env_port[1024];
				snprintf(env_port, 1024, "SERVER_PORT=%hu", server->params->port);
				
				char env_address[1024];
				snprintf(env_address, 1024, "REMOTE_ADDR=%s", client->address);
				
				char* envp[] =
				{
					env_selector,
					env_query,
					env_hostname,
					env_port,
					env_address,
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
			
			// There's no need for the client's file to be open at this point
			close(client->file);
			client->file = -1;
			
			// Alter the events on the client socket to only handle errors
			sepoll_mod_events(server->loop, fd, EPOLLET);
			
			// Add the pidfd to the event loop
			sepoll_add(server->loop, client->pidfd, EPOLLIN, client_pidfd, server, client);
		}
		else
		{
			// Otherwise, transmit the file
			client->filesize = statbuf.st_size;
			sepoll_mod_events(server->loop, fd, EPOLLOUT | EPOLLET);
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
			ssize_t n = sendfile(fd, client->file, &client->sentsize, (size_t)(client->filesize - client->sentsize));
			
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
						write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
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
					fprintf(stderr, "%i - Error: Cannot accept incoming connection: %s\n", getpid(), strerror(errno));
					return;
				}
			}
			
			// Check if we have already hit the maximum number of clients
			if (server->numClients == server->params->maxClients)
			{
				// Server's full
				write(fd, ERROR_UNAVAILABLE, sizeof(ERROR_UNAVAILABLE) - 1);
				close(client_fd);
				continue;
			}
			
			// Allocate a new client data structure
			struct client_state* client = malloc(sizeof(struct client_state));
			
			if (client == NULL)
			{
					write(fd, ERROR_INTERNAL, sizeof(ERROR_INTERNAL) - 1);
					close(client_fd);
					continue;
			}
			
			// Initialize the client, add their socket FD to the watch list, and add the client to the list
			client->socket = client_fd;
			client->timestamp = time(NULL);
			client->count = 0;
			client->file = -1;
			client->sentsize = 0;
			client->dirfd = -1;
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
				fprintf(stderr, "%i - Error: Cannot read from signalfd: %s\n", getpid(), strerror(errno));
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
	
	// Have to read 8 bytes from the timer to reset it even if I don't care about the contents
	uint64_t buffer;
	
	if (read(fd, &buffer, sizeof(uint64_t)) != sizeof(uint64_t))
	{
		fprintf(stderr, "%i - Error: Cannot read from timerfd: %s\n", getpid(), strerror(errno));
	}
	
	// Check for connection timeout
	time_t currentTime = time(NULL);

	struct client_state* client = LIST_FIRST(&server->clients);
	
	while (client != NULL)
	{
		struct client_state* next = LIST_NEXT(client, entry);
		
		if (client->pidfd >= 0)
		{
			// We don't have timestamps for CGI process interactions so we need to spy on the TCP connection information
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
	if (sigaction(SIGPIPE, &act, NULL) < 0)
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
		
		// Kill CGI process, if any
		if (client->pidfd >= 0)
		{
			pidfd_kill(client->pidfd);
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
	close(server.socket);
	close(server.timerfd);
	close(server.sigfd);
	close(server.directory);
	
	return 0;
}
