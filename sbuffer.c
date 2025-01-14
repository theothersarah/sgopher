// errno
#include <errno.h>

// poll
#include <poll.h>

// va_start, va_end
#include <stdarg.h>

// vsnprintf
#include <stdio.h>

// write
#include <unistd.h>

// definitions
#include "sbuffer.h"

int sbuffer_push(struct sbuffer_t* sbuffer, const char* fmt, ...)
{
	va_list ap;
	
	va_start(ap, fmt);
	int n = vsnprintf(sbuffer->pos, sbuffer_remaining(sbuffer), fmt, ap);
	va_end(ap);
	
	if (n > 0)
	{
		// >= is used to take the implied null terminator into account: if size == buf->size, one character is actually cut off in favor of the null
		if ((size_t)n >= sbuffer_remaining(sbuffer))
		{
			// Not enough room to fit it
			return 0;
		}
		else
		{
			// Update position
			sbuffer->pos += n;
		}
	}
	else if (n < 0)
	{
		return -errno;
	}
	
	return n;
}

int sbuffer_flush(struct sbuffer_t* sbuffer)
{
	// Potentially requires multiple writes especially if the FD is a socket, pipe, etc.
	while (sbuffer_unwritten(sbuffer) > 0)
	{
		ssize_t n = write(sbuffer->fd, sbuffer->writepos, sbuffer_unwritten(sbuffer));
		
		if (n < 0)
		{
			if (errno == EAGAIN)
			{
				// If the FD is non-blocking and would block, poll it before trying again
				struct pollfd fds =
				{
					.fd = sbuffer->fd,
					.events = POLLOUT
				};
				
				int retval = poll(&fds, 1, sbuffer->timeout);
				
				if (retval < 0)
				{
					// Poll error
					return -errno;
				}
				else if (retval == 0)
				{
					// Poll timeout
					return -EAGAIN;
				}
				
				// Try again
				continue;
			}
			else
			{
				// Error writing
				return -errno;
			}
		}
		
		// Update position and write counter
		sbuffer->writepos += n;
		sbuffer->written += (size_t)n;
	}
	
	// Reset buffer
	sbuffer->pos = sbuffer->base;
	sbuffer->writepos = sbuffer->base;
	
	return 0;
}
