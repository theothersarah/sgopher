// errno
#include <errno.h>

// NULL, size_t
#include <stddef.h>

// mmap, munmap
#include <sys/mman.h>

// *********************************************************************
// Shared memory allocation functions
//
// smalloc asks for a little extra memory so it can prepend a size,
// which is used by sfree when later being freed. scalloc is a wrapper
// around smalloc which is for allocating an array.
// *********************************************************************

void* smalloc(size_t size)
{
	if (size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t length = sizeof(size_t) + size;

	size_t* ptr = (size_t*)mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (ptr == MAP_FAILED)
	{
		return NULL;
	}

	*ptr = length;
	ptr++;

	return (void*)ptr;
}

void* scalloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	size_t total = nmemb * size;
	
	if (total / nmemb != size)
	{
		errno = EOVERFLOW;
		return NULL;
	}
	
	return smalloc(total);
}

void sfree(void* ptr)
{
	if (ptr == NULL)
	{
		return;
	}

	size_t* base = (size_t*)ptr;
	base--;

	munmap((void*)base, *base);
}
