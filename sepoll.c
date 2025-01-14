// for tdestroy
#define _GNU_SOURCE

// errno
#include <errno.h>

// tsearch, tdelete, tfind, tdestroy
#include <search.h>

// bool
#include <stdbool.h>

// malloc, calloc, reallocarray, free
#include <stdlib.h>

// epoll API
#include <sys/epoll.h>

// Linked list macros
#include <sys/queue.h>

// close
#include <unistd.h>

// sepoll_arg_t
# include "sepoll.h"

// *********************************************************************
// Core definitions
// *********************************************************************

struct sepoll_callback_t
{
	// Associated file descriptor, used as the key for tree searching
	int fd;
	
	// Function pointer and userdata arguments
	void (*function)(uint32_t, union sepoll_arg_t, union sepoll_arg_t);
	
	union sepoll_arg_t userdata1;
	union sepoll_arg_t userdata2;
	
	// Can also be added to a list while awaiting garbage collection
	LIST_ENTRY(sepoll_callback_t) entry;
};

LIST_HEAD(sepoll_callback_list_t, sepoll_callback_t);

struct sepoll_t
{
	// Tree for active callbacks, sorted by file descriptor
	void* callbacks_tree;
	
	// List for garbage collection since order is not important for that
	struct sepoll_callback_list_t callbacks_gc_list;
	
	// epoll stuff
	struct epoll_event* epoll_events;
	int epoll_events_size;
	int epollfd;
	
	// Set to false during looping to exit the loop
	bool run;
};

// *********************************************************************
// Functions for tree searching
// *********************************************************************

static int compare_callback_fd(const void* pa, const void* pb)
{
	const struct sepoll_callback_t* a = pa;
	const struct sepoll_callback_t* b = pb;
	
	return a->fd - b->fd;
}

static struct sepoll_callback_t* sepoll_find_fd(struct sepoll_t* loop, int fd)
{
	struct sepoll_callback_t searchfd =
	{
		.fd = fd
	};
	
	struct sepoll_callback_t** found = tfind(&searchfd, &loop->callbacks_tree, compare_callback_fd);
	
	if (found == NULL)
	{
		return NULL;
	}
	
	return *found;
}

// *********************************************************************
// Creation, resizing, and destruction functions
//
// Size is actually how many events can be returned in one loop, rather
// than how many events can be registered in total. Ideally it should be
// greater than the average number of events expected to occur
// simultaneously.
// *********************************************************************

struct sepoll_t* sepoll_create(int size, int flags)
{
	// Allocate memory for opaque structure
	struct sepoll_t* loop = malloc(sizeof(struct sepoll_t));
	
	if (loop == NULL)
	{
		return NULL;
	}
	
	// Allocate memory for returned epoll events
	loop->epoll_events = calloc((size_t)size, sizeof(struct epoll_event));
	
	if (loop->epoll_events == NULL)
	{
		free(loop);
		return NULL;
	}
	
	// Create the epoll fd with the almost-always desirable CLOEXEC flag
	loop->epollfd = epoll_create1(flags);
	
	if (loop->epollfd < 0)
	{
		free(loop->epoll_events);
		free(loop);
		return NULL;
	}
	
	//  Initialize
	loop->callbacks_tree = NULL;
	
	LIST_INIT(&loop->callbacks_gc_list);
	
	loop->epoll_events_size = size;
	
	return loop;
}

int sepoll_resize(struct sepoll_t* loop, int size)
{
	if (loop == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	
	if (size <= 0)
	{
		errno = EINVAL;
		return -1;
	}
	
	if (size == loop->epoll_events_size)
	{
		return 0;
	}
	
	void* ptr = reallocarray(loop->epoll_events, (size_t)size, sizeof(struct epoll_event));
	
	if (ptr == NULL)
	{
		return -1;
	}
	
	loop->epoll_events = ptr;
	loop->epoll_events_size = size;
	
	return 0;
}

void sepoll_destroy(struct sepoll_t* loop)
{
	if (loop == NULL)
	{
		return;
	}
	
	if (loop->callbacks_tree != NULL)
	{
		tdestroy(loop->callbacks_tree, free);
	}
	
	struct sepoll_callback_t* callback = LIST_FIRST(&loop->callbacks_gc_list);
	
	while (callback != NULL)
	{
		struct sepoll_callback_t* next = LIST_NEXT(callback, entry);
	
		free(callback);
		
		callback = next;
	}
	
	close(loop->epollfd);
	
	free(loop->epoll_events);
	
	free(loop);
}

// *********************************************************************
// Add, modify, and remove an event
// *********************************************************************

// Add an FD to the poll list
int sepoll_add(struct sepoll_t* loop, int fd, uint32_t events, void (*function)(uint32_t, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	// Allocate memory for the callback and initialize it
	struct sepoll_callback_t* callback = malloc(sizeof(struct sepoll_callback_t));
	
	if (callback == NULL)
	{
		return -1;
	}
	
	callback->fd = fd;
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	// Attempt to add it to the tree
	// If it can't be added or something with this file descriptor already exists on it, fail
	struct sepoll_callback_t** entry = tsearch(callback, &loop->callbacks_tree, compare_callback_fd);
	
	if (entry == NULL)
	{
		free(callback);
		errno = ENOMEM;
		return -1;
	}
	else if (*entry != callback)
	{
		free(callback);
		errno = EEXIST;
		return -1;
	}
	
	// Add the event to the epoll instance
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	if (epoll_ctl(loop->epollfd, EPOLL_CTL_ADD, fd, &event) < 0)
	{
		tdelete(callback, &loop->callbacks_tree, compare_callback_fd);
		free(callback);
		return -1;
	}
	
	return 0;
}

// Modify event mask, callback function, and userdata for a polled FD
int sepoll_mod(struct sepoll_t* loop, int fd, uint32_t events, void (*function)(uint32_t, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct sepoll_callback_t* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		errno = EBADF;
		return -1;
	}
	
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	if (epoll_ctl(loop->epollfd, EPOLL_CTL_MOD, fd, &event) < 0)
	{
		return -1;
	}
	
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	return 0;
}

// Change only the event mask for a polled FD
int sepoll_mod_events(struct sepoll_t* loop, int fd, uint32_t events)
{
	struct sepoll_callback_t* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		errno = EBADF;
		return -1;
	}
	
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	return epoll_ctl(loop->epollfd, EPOLL_CTL_MOD, fd, &event);
}

// Change only the callback function and userdata for a polled FD
int sepoll_mod_callback(struct sepoll_t* loop, int fd, void (*function)(uint32_t, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2)
{
	struct sepoll_callback_t* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		errno = EBADF;
		return -1;
	}
	
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	return 0;
}

// Remove an FD from the poll list
int sepoll_remove(struct sepoll_t* loop, int fd)
{
	// Get the event data associated with the file descriptor
	struct sepoll_callback_t* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		errno = EBADF;
		return -1;
	}
	
	// Null out the callback function so it doesn't get called if it's still in the queue
	callback->function = NULL;
	
	// Remove the event from the active tree and add it to the garbage collection list so it can be freed later
	tdelete(callback, &loop->callbacks_tree, compare_callback_fd);
	LIST_INSERT_HEAD(&loop->callbacks_gc_list, callback, entry);
	
	// Remove the file descriptor from epoll's interest list
	return epoll_ctl(loop->epollfd, EPOLL_CTL_DEL, fd, NULL);
}

// *********************************************************************
// Functions for entering the event loop
// *********************************************************************

// Enter the event loop until sepoll_exit is called within a callback function
int sepoll_enter(struct sepoll_t* loop, int timeout, void (*function)(int, void*), void* userdata)
{
	int retval = 0;
	
	loop->run = true;
	
	while (loop->run)
	{
		// Wait on epoll events or a timeout
		int n = epoll_wait(loop->epollfd, loop->epoll_events, loop->epoll_events_size, timeout);
		
		if (n > 0)
		{
			// Iterate over returned events and call the callback functions
			for (int i = 0; i < n; i++)
			{
				struct sepoll_callback_t* callback = loop->epoll_events[i].data.ptr;
				
				if (callback->function != NULL)
				{
					callback->function(loop->epoll_events[i].events, callback->userdata1, callback->userdata2);
				}
			}
		}
		
		// Execute callback function if one was provided
		if (function != NULL)
		{
			function(n, userdata);
		}
		else if (n == 0)
		{
			// Timeout occurred without a provided callback function
			loop->run = false;
		}
		else if (n < 0)
		{
			// Error occurred without a provided callback function
			retval = -1;
			loop->run = false;
		}
		
		// Iterate through the garbage collection list and free everything in it
		struct sepoll_callback_t* callback = LIST_FIRST(&loop->callbacks_gc_list);
		
		while (callback != NULL)
		{
			struct sepoll_callback_t* next = LIST_NEXT(callback, entry);
			
			LIST_REMOVE(callback, entry);
			
			free(callback);
			
			callback = next;
		}
	}
	
	return retval;
}

// Exit the event loop
void sepoll_exit(struct sepoll_t* loop)
{
	loop->run = false;
}
