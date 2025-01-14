#pragma once

struct sbuffer_t
{
	// file descriptor to flush the buffer to
	int fd;
	
	// Timeout for poll if write is non-blocking and would block
	int timeout;
	
	// Base address and total length of buffer
	char* base;
	size_t len;
	
	// Current positions for adding and writing
	char* pos;
	char* writepos;
	
	// Counter for total bytes written
	size_t written;
};

static inline void sbuffer_init(struct sbuffer_t* sbuffer, int fd, int timeout, char* base, size_t len)
{
	sbuffer->fd = fd;
	
	sbuffer->timeout = timeout;
	
	sbuffer->base = base;
	sbuffer->len = len;
	
	sbuffer->pos = base;
	sbuffer->writepos = base;
	
	sbuffer->written = 0;
}

int sbuffer_push(struct sbuffer_t* sbuffer, const char* fmt, ...);
int sbuffer_flush(struct sbuffer_t* sbuffer);

static inline size_t sbuffer_remaining(struct sbuffer_t* sbuffer)
{
	return sbuffer->len - (size_t)(sbuffer->pos - sbuffer->base);
}

static inline size_t sbuffer_unwritten(struct sbuffer_t* sbuffer)
{
	return (size_t)(sbuffer->pos - sbuffer->writepos);
}

static inline int sbuffer_checkflush(struct sbuffer_t* sbuffer, size_t leftover)
{
	if (sbuffer_remaining (sbuffer) < leftover)
	{
		return sbuffer_flush(sbuffer);
	}
	
	return 0;
}
