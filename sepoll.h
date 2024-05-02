#pragma once

#include <sys/epoll.h>

struct sepoll_loop;

//
struct sepoll_loop* sepoll_create(int size);
int sepoll_resize(struct sepoll_loop* loop, int size);
void sepoll_destroy(struct sepoll_loop* loop);

// Polling
int sepoll_add(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod(struct sepoll_loop* loop, int fd, unsigned int events, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_mod_events(struct sepoll_loop* loop, int fd, unsigned int events);
int sepoll_mod_userdata(struct sepoll_loop* loop, int fd, void (*function)(int, unsigned int, void*, void*), void* userdata1, void* userdata2);
int sepoll_remove(struct sepoll_loop* loop, int fd, int close);

//
void sepoll_queue_close(struct sepoll_loop* loop, int fd);

//
int sepoll_enter(struct sepoll_loop* loop);
void sepoll_exit(struct sepoll_loop* loop);
int sepoll_once(struct sepoll_loop* loop, int timeout);
