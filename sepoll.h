#pragma once

// For epoll event constants
#include <sys/epoll.h>

// Opaque structure for event loop state
struct sepoll_t;

// Userdata type for function arguments, matching the types that the epoll user data accommodates
// Implemented as a transparent union so that any of these types can be provided as the function arguments
union sepoll_arg_t
{
	uint64_t u64;
	uint32_t u32;
	int fd;
	void* ptr;
} __attribute__((__transparent_union__));

// Lifecycle management - creation, resizing, and destruction
struct sepoll_t* sepoll_create(int size);
int sepoll_resize(struct sepoll_t* loop, int size);
void sepoll_destroy(struct sepoll_t* loop);

// Add, modify, and remove callbacks
int sepoll_add(struct sepoll_t* loop, int fd, uint32_t events, void (*function)(unsigned int, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2);
int sepoll_mod(struct sepoll_t* loop, int fd, uint32_t events, void (*function)(unsigned int, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2);
int sepoll_mod_events(struct sepoll_t* loop, int fd, uint32_t events);
int sepoll_mod_callback(struct sepoll_t* loop, int fd, void (*function)(unsigned int, union sepoll_arg_t, union sepoll_arg_t), union sepoll_arg_t userdata1, union sepoll_arg_t userdata2);
int sepoll_remove(struct sepoll_t* loop, int fd);

// Event loop management
int sepoll_enter(struct sepoll_t* loop);
void sepoll_exit(struct sepoll_t* loop);
int sepoll_once(struct sepoll_t* loop, int timeout);
