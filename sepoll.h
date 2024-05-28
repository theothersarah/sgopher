#pragma once

// For epoll event constants
#include <sys/epoll.h>

struct sepoll_loop;

// Lifecycle management - creation, resizing, and destruction
struct sepoll_loop* sepoll_create(int size);
int sepoll_resize(struct sepoll_loop* loop, int size);
void sepoll_destroy(struct sepoll_loop* loop);

// Add, modify, and remove callbacks
int sepoll_add(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod_events(struct sepoll_loop* loop, int fd, unsigned int events);
int sepoll_mod_userdata(struct sepoll_loop* loop, int fd, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_remove(struct sepoll_loop* loop, int fd);

// Event loop management
int sepoll_enter(struct sepoll_loop* loop);
void sepoll_exit(struct sepoll_loop* loop);
int sepoll_once(struct sepoll_loop* loop, int timeout);
