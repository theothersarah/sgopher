// for tdestroy
#define _GNU_SOURCE

// io_uring API
#include <liburing.h>

// tsearch, tdelete, tfind, tdestroy
#include <search.h>

// malloc, calloc, reallocarray, free
#include <stdlib.h>

// epoll API
#include <sys/epoll.h>

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
};

struct sepoll_loop
{
	void* callbacks_tree;
	void* callbacks_gc_tree;
	
	struct epoll_event* epoll_events;
	int epoll_events_size;
	int epollfd;
	
	struct epoll_event* uring_epoll_events;
	int uring_event_size;
	int uring_epoll_event_index;
	struct io_uring uring;
	
	int run;
};

// *********************************************************************
// Compare functions for tree searches
// *********************************************************************

static int compare_pointer(const void* pa, const void* pb)
{
	if (pa < pb)
	{
		return -1;
	}
	else if (pa > pb)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

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
	loop->uring_epoll_events = calloc((size_t)size, sizeof(struct epoll_event));
	
	if (loop->uring_epoll_events == NULL)
	{
		free(loop->epoll_events);
		free(loop);
		return NULL;
	}
	
	//
	loop->epollfd = epoll_create1(EPOLL_CLOEXEC);
	
	if (loop->epollfd < 0)
	{
		free(loop->uring_epoll_events);
		free(loop->epoll_events);
		free(loop);
		return NULL;
	}
	
	if (io_uring_queue_init(4096, &loop->uring, 0) < 0)
	{
		close(loop->epollfd);
		free(loop->uring_epoll_events);
		free(loop->epoll_events);
		free(loop);
		return NULL;
	}
	
	//
	loop->callbacks_tree = NULL;
	loop->callbacks_gc_tree = NULL;
	loop->epoll_events_size = size;
	
	loop->uring_event_size = size;
	loop->uring_epoll_event_index = 0;
	
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
	
	if (loop->callbacks_gc_tree != NULL)
	{
		tdestroy(loop->callbacks_gc_tree, free);
	}
	
	io_uring_queue_exit(&loop->uring);
	
	close(loop->epollfd);
	
	free(loop->epoll_events);
	free(loop->uring_epoll_events);
	
	free(loop);
}

// *********************************************************************
// 
// *********************************************************************

static inline void sepoll_process_uring(struct sepoll_loop* loop)
{
	//
	io_uring_submit_and_get_events(&loop->uring);
	
	unsigned int n = io_uring_cq_ready(&loop->uring);
	
	while (n > 0)
	{
		io_uring_cq_advance(&loop->uring, n);
		
		if (io_uring_cq_has_overflow(&loop->uring) != 0)
		{
			io_uring_get_events(&loop->uring);
		}
		
		n = io_uring_cq_ready(&loop->uring);
	}
	
	loop->uring_epoll_event_index = 0;
}

static inline void sepoll_process_if_less(struct sepoll_loop* loop, unsigned int n)
{
	if (io_uring_sq_space_left(&loop->uring) < n)
	{
		sepoll_process_uring(loop);
	}
}

// *********************************************************************
// 
// *********************************************************************

static inline void sepoll_epollctl(struct sepoll_loop* loop, int op, int fd, struct epoll_event* event, int close)
{
	sepoll_process_if_less(loop, 2);
	
	struct epoll_event* sqe_event = NULL;
	
	if (op != EPOLL_CTL_DEL)
	{
		loop->uring_epoll_events[loop->uring_epoll_event_index].events = event->events;
		loop->uring_epoll_events[loop->uring_epoll_event_index].data.ptr = event->data.ptr;
		
		sqe_event = &loop->uring_epoll_events[loop->uring_epoll_event_index];
		
		loop->uring_epoll_event_index++;
	}
	
	struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->uring);
	io_uring_prep_epoll_ctl(sqe, loop->epollfd, fd, op, sqe_event);

	if (close)
	{
		io_uring_sqe_set_flags(sqe, IOSQE_IO_HARDLINK);
		sqe = io_uring_get_sqe(&loop->uring);
		io_uring_prep_close(sqe, fd);
	}
}

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
	
	sepoll_epollctl(loop, EPOLL_CTL_ADD, fd, &event, 0);
	
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
	
	callback->function = function;
	callback->userdata1 = userdata1;
	callback->userdata2 = userdata2;
	
	struct epoll_event event =
	{
		.events = events,
		.data.ptr = callback
	};
	
	sepoll_epollctl(loop, EPOLL_CTL_MOD, fd, &event, 0);
	
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
	
	sepoll_epollctl(loop, EPOLL_CTL_MOD, fd, &event, 0);
	
	return 0;
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

int sepoll_remove(struct sepoll_loop* loop, int fd, int close)
{
	// Get the event data associated with the file descriptor
	struct sepoll_callback* callback = sepoll_find_fd(loop, fd);
	
	if (callback == NULL)
	{
		return -1;
	}
	
	// Null out the callback function so it doesn't get called if it's still in the queue
	callback->function = NULL;
	
	// Remove the event from the active tree and add it to the garbage collection tree
	tdelete(callback, &loop->callbacks_tree, compare_callback_fd);
	tsearch(callback, &loop->callbacks_gc_tree, compare_pointer);
	
	// Remove the file descriptor from epoll's interest list
	sepoll_epollctl(loop, EPOLL_CTL_DEL, fd, NULL, close);
	
	return 0;
}

// *********************************************************************
// 
// *********************************************************************

void sepoll_queue_close(struct sepoll_loop* loop, int fd)
{
	sepoll_process_if_less(loop, 1);
	
	struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->uring);
	io_uring_prep_close(sqe, fd);
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
	
	// 
	if (loop->callbacks_gc_tree != NULL)
	{
		tdestroy(loop->callbacks_gc_tree, free);
		loop->callbacks_gc_tree = NULL;
	}
	
	//
	if (io_uring_sq_ready(&loop->uring) > 0)
	{
		sepoll_process_uring(loop);
	}
	
	return n;
}

int sepoll_enter(struct sepoll_loop* loop)
{
	sepoll_process_uring(loop);
	
	loop->run = 1;
	
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
	loop->run = 0;
}

int sepoll_once(struct sepoll_loop* loop, int timeout)
{
	sepoll_process_uring(loop);
	
	return sepoll_iter(loop, timeout);
}
