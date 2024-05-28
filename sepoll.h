#pragma once

// For epoll event constants
#include <sys/epoll.h>

struct sepoll_t;

// Lifecycle management - creation, resizing, and destruction
struct sepoll_t* sepoll_create(int size);
int sepoll_resize(struct sepoll_t* loop, int size);
void sepoll_destroy(struct sepoll_t* loop);

// Add, modify, and remove callbacks
int sepoll_add(struct sepoll_t* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod(struct sepoll_t* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod_events(struct sepoll_t* loop, int fd, unsigned int events);
int sepoll_mod_userdata(struct sepoll_t* loop, int fd, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_remove(struct sepoll_t* loop, int fd);

// Event loop management
int sepoll_enter(struct sepoll_t* loop);
void sepoll_exit(struct sepoll_t* loop);
int sepoll_once(struct sepoll_t* loop, int timeout);
