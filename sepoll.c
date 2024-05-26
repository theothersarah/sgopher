// for tdestroy
#define _GNU_SOURCE

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

// *********************************************************************
// Core definitions
// *********************************************************************

struct sepoll_callback
{
	int fd;
	
	void (*function)(int, unsigned int, void*, void*);
	
	void* userdata1;
	void* userdata2;
	
	LIST_ENTRY(sepoll_callback) entry;
};

LIST_HEAD(callback_list, sepoll_callback);

struct sepoll_loop
{
	void* callbacks_tree;
	struct callback_list callbacks_gc_list;
	
	struct epoll_event* epoll_events;
	int epoll_events_size;
	int epollfd;
	
	bool run;
};

// *********************************************************************
// Compare functions for tree searches
// *********************************************************************

static int compare_callback_fd(const void* pa, const void* pb)
{
	const struct sepoll_callback* a = pa;
	const struct sepoll_callback* b = pb;
	
	if (a->fd < b->fd)
	{
		return -1;
	}
	else if (a->fd > b->fd)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

// *********************************************************************
// Creation, resizing, and destruction functions
// *********************************************************************

struct sepoll_loop* sepoll_create(int size)
{
	//
	struct sepoll_loop* loop = malloc(sizeof(struct sepoll_loop));
	
	if (loop == NULL)
	{
		return NULL;
	}
	
	//
	loop->epoll_events = calloc((size_t)size, sizeof(struct epoll_event));
	
	if (loop->epoll_events == NULL)
	{
		free(loop);
		return NULL;
	}
	
	//
	loop->epollfd = epoll_create1(EPOLL_CLOEXEC);
	
	if (loop->epollfd < 0)
	{
		free(loop->epoll_events);
		free(loop);
		return NULL;
	}
	
	// 
	loop->callbacks_tree = NULL;
	
	LIST_INIT(&loop->callbacks_gc_list);
	
	loop->epoll_events_size = size;
	
	return loop;
}

int sepoll_resize(struct sepoll_loop* loop, int size)
{
	if (loop == NULL)
	{
		return -1;
	}
	
	if (size <= 0)
	{
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

void sepoll_destroy(struct sepoll_loop* loop)
{
	if (loop == NULL)
	{
		return;
	}
	
	if (loop->callbacks_tree != NULL)
	{
		tdestroy(loop->callbacks_tree, free);
	}
	
	struct sepoll_callback* callback = LIST_FIRST(&loop->callbacks_gc_list);
	
	while (callback != NULL)
	{
		struct sepoll_callback* next = LIST_NEXT(callback, entry);
	
		free(callback);
		
		callback = next;
	}
	
	close(loop->epollfd);
	
	free(loop->epoll_events);
	
	free(loop);
}

// *********************************************************************
// 
// *********************************************************************

int sepoll_add(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2)
{
	struct sepoll_callback* callback = malloc(sizeof(struct sepoll_callback));
	
	if (callback == NULL)
	{
		return -1;
	}
	
	//
	callback->fd = fd;
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	//
	struct sepoll_callback** entry = tsearch(callback, &loop->callbacks_tree, compare_callback_fd);
	
	if (entry == NULL || *entry != callback)
	{
		free(callback);
		return -1;
	}
	
	//
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	if (epoll_ctl(loop->epollfd, EPOLL_CTL_ADD, fd, &event) < 0)
	{
		free(callback);
		return -1;
	}
	
	return 0;
}

static struct sepoll_callback* sepoll_find_fd(struct sepoll_loop* loop, int fd)
{
	struct sepoll_callback searchfd =
	{
		.fd = fd
	};
	
	struct sepoll_callback** found = tfind(&searchfd, &loop->callbacks_tree, compare_callback_fd);
	
	if (found == NULL)
	{
		return NULL;
	}
	
	return *found;
}

int sepoll_mod(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2)
{
	struct sepoll_callback* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
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

int sepoll_mod_events(struct sepoll_loop* loop, int fd, unsigned int events)
{
	struct sepoll_callback* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		return -1;
	}
	
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	return epoll_ctl(loop->epollfd, EPOLL_CTL_MOD, fd, &event);
}

int sepoll_mod_userdata(struct sepoll_loop* loop, int fd, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2)
{
	struct sepoll_callback* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		return -1;
	}
	
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	return 0;
}

int sepoll_remove(struct sepoll_loop* loop, int fd)
{
	// Get the event data associated with the file descriptor
	struct sepoll_callback* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		return -1;
	}
	
	// Null out the callback function so it doesn't get called if it's still in the queue
	callback->function = NULL;
	
	// Remove the event from the active tree and add it to the garbage collection list
	tdelete(callback, &loop->callbacks_tree, compare_callback_fd);
	LIST_INSERT_HEAD(&loop->callbacks_gc_list, callback, entry);
	
	// Remove the file descriptor from epoll's interest list
	return epoll_ctl(loop->epollfd, EPOLL_CTL_DEL, fd, NULL);
}

// *********************************************************************
// 
// *********************************************************************

static inline int sepoll_iter(struct sepoll_loop* loop, int timeout)
{
	//
	int n = epoll_wait(loop->epollfd, loop->epoll_events, loop->epoll_events_size, timeout);
	
	if (n < 0)
	{
		return -1;
	}
	
	// Iterate over returned events and call the callback functions
	for (int i = 0; i < n; i++)
	{
		struct sepoll_callback* callback = loop->epoll_events[i].data.ptr;
		
		if (callback->function != NULL)
		{
			callback->function(callback->fd, loop->epoll_events[i].events, callback->userdata1, callback->userdata2);
		}
	}
	
	// Iterate through the garbage collection list and free everything in it
	struct sepoll_callback* callback = LIST_FIRST(&loop->callbacks_gc_list);
	
	while (callback != NULL)
	{
		struct sepoll_callback* next = LIST_NEXT(callback, entry);
		
		LIST_REMOVE(callback, entry);
		
		free(callback);
		
		callback = next;
	}
	
	return n;
}

int sepoll_enter(struct sepoll_loop* loop)
{
	loop->run = true;
	
	do
	{
		int retval = sepoll_iter(loop, -1);
		
		if (retval < 0)
		{
			return retval;
		}
	}
	while (loop->run);
	
	return 0;
}

void sepoll_exit(struct sepoll_loop* loop)
{
	loop->run = false;
}

int sepoll_once(struct sepoll_loop* loop, int timeout)
{
	return sepoll_iter(loop, timeout);
}
