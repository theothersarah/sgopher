// opendir, readdir, closedir
#include <dirent.h>

// errno
#include <errno.h>

// snprintf, fprintf
#include <stdio.h>

// atexit, getenv, malloc, calloc, reallocarray, free, qsort, bsearch
#include <stdlib.h>

// strerror, strcpy, strchr, strlen, strncat, strcat, strrchr, strcmp
#include <string.h>

// stat
#include <sys/stat.h>

// write, getpid
#include <unistd.h>

// Buffering for several snprintf calls to limit the number of writes performed
#define BUFFER_LENGTH 65536
#define BUFFER_BUFFER 4096

struct snbuffer
{
	// file descriptor to flush the buffer to
	int fd;
	
	// Base and length of buffer
	char* base;
	size_t len;
	
	// These are the current end position and remaining size of the buffer
	// They are passed to snprintf and updated by snbuffer_push
	char* pos;
	size_t size;
};

static inline void snbuffer_setup(struct snbuffer* buf, int fd, char* base, size_t len)
{
	buf->fd = fd;
	
	buf->base = base;
	buf->len = len;
	
	buf->pos = base;
	buf->size = len;
}

void snbuffer_flush(struct snbuffer* buf)
{
	size_t count = (size_t)(buf->pos - buf->base);
	
	if (count > 0)
	{
		// Potentially requires multiple writes especially if the FD is a socket
		char* from = buf->base;
		
		do
		{
			ssize_t n = write(buf->fd, from, count);
			
			if (n < 0)
			{
				fprintf(stderr, "%i (gopherlist) - Error: Cannot write: %s\n", getpid(), strerror(errno));
				exit(EXIT_FAILURE);
			}
			
			count -= (size_t)n;
			from += n;
		}
		while(count > 0);
		
		// Reset buffer
		buf->pos = buf->base;
		buf->size = buf->len;
	}
}

void snbuffer_push(struct snbuffer* buf, size_t leftover, int size)
{
	if (size > 0)
	{
		// >= is used to take the implied null terminator into account: if size == buf->size, one character is actually cut off in favor of the null
		if ((size_t)size >= buf->size)
		{
			fprintf(stderr, "%i (gopherlist) - Warning: Discarded snprintf result due to size\n", getpid());
		}
		else
		{
			// Update position and remaining size for next call
			buf->pos += size;
			buf->size -= (size_t)size;
		}
	}
	else if (size < 0)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot snprintf: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Check if a flush is necessary to free space for the next call
	if (buf->size < leftover)
	{
		snbuffer_flush(buf);
	}
}

// List of filenames to be freed on exit
static size_t filenames_count = 0;
static char** filenames = NULL;

static void cleanup()
{
	if (filenames != NULL)
	{
		for (size_t i = 0; i < filenames_count; i++)
		{
			free(filenames[i]);
		}
		
		free(filenames);
	}
}

// Comparison function for filename list
static int compare_strings(const void* pa, const void* pb)
{
	return strcmp(*((const char**)pa), *((const char**)pb));
}

// Mapping of extension to selector type
// Not comprehensive but at least an assortment of common and period-accurate stuff
// This must be pre-sorted according to strcmp rules for bsearch
// Default for a non-executable, non-directory file is 9 so anything of that type should not be here

struct ext_entry
{
	const char* ext;
	const char type;
};

static const struct ext_entry ext_table[] =
{
	{"bmp", 'I'},
	{"c", '0'},
	{"cpp", '0'},
	{"gif", 'g'},
	{"h", '0'},
	{"htm", 'h'},
	{"html", 'h'},
	{"jpeg", 'I'},
	{"jpg", 'I'},
	{"mp3", 's'},
	{"ogg", 's'},
	{"pcx", 'I'},
	{"png", 'I'},
	{"tif", 'I'},
	{"tiff", 'I'},
	{"txt", '0'},
	{"wav", 's'}
};

// Comparison function for the file extension list
static int compare_ext_entry(const void* pa, const void* pb)
{
	const struct ext_entry* entry1 = pa;
	const struct ext_entry* entry2 = pb;
	
	return strcmp(entry1->ext, entry2->ext);
}

// Main function
int main()
{
	atexit(cleanup);
	
	// Get the environment variables we need
	// It's fine if they are null, too, even if it would generate a non-functional menu
	char* env_selector = getenv("SCRIPT_NAME");
	char* env_hostname = getenv("SERVER_NAME");
	char* env_port = getenv("SERVER_PORT");
	
	// Trim extra slashes out of the selector. Multiple slashes in a row are valid so this is mostly cosmetic.
	// Also makes sure the selector has a slash at the beginning and end which are also not strictly necessary,
	// but play nice with adding the hostname and filename to the front and end respectively.
	// Also makes it easier to generate the selector for the "up a level" link.
	// The passed selector from sgopher should not exceed 510 bytes so extra room is allocated and no length
	// checking should be necessary
	char selector[1024];
	strcpy(selector, "/");
	
	int n = 0;
	
	// Should only inspect the string if it is not null and has a non-zero length
	if (env_selector != NULL && env_selector[0] != '\0')
	{
		char* str_curr = env_selector;
		
		do
		{
			char* slash = strchr(str_curr, '/');
			
			size_t str_size;
			
			if (slash == NULL)
			{
				str_size = strlen(str_curr);
			}
			else
			{
				str_size = (size_t)(slash - str_curr);
			}
			
			if (str_size > 0)
			{
				n++;
				strncat(selector, str_curr, str_size);
				strcat(selector, "/");
			}
			
			str_curr = slash;
		}
		while (str_curr++ != NULL);
	}
	
	// Allocate the list of filenames with a starter size that may be increased later
	size_t filenames_size = 256;
	
	filenames = calloc(filenames_size, sizeof(char*));
	
	if (filenames == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filenames: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Open a directory stream for the working directory so we can go through its files and add them to the list
	DIR* directory = opendir(".");
	
	if (directory == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot opendir: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	struct dirent* entry;
	
	while (entry = readdir(directory), entry != NULL)
	{
		// Ignore hidden files
		if (entry->d_name[0] == '.')
		{
			continue;
		}
		
		// Make a copy of the filename
		char* filename = malloc(strlen(entry->d_name) + 1);
		
		if (filename == NULL)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename: %s\n", getpid(), strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		strcpy(filename, entry->d_name);
		
		// Add the filename to the list
		filenames[filenames_count] = filename;
		
		filenames_count++;
		
		// Resize the list if necessary
		if (filenames_count == filenames_size)
		{
			filenames_size *= 2;
			
			filenames = reallocarray(filenames, filenames_size, sizeof(char*));
			
			if (filenames == NULL)
			{
				fprintf(stderr, "%i (gopherlist) - Error: Cannot reallocate memory for filenames: %s\n", getpid(), strerror(errno));
				exit(EXIT_FAILURE);
			}
		}
	}
	
	closedir(directory);
	
	// Sort the list of filenames
	qsort(filenames, filenames_count, sizeof(char*), compare_strings);
	
	// Prepare a buffer for the output to produce a minimum number of writes
	char buffer[BUFFER_LENGTH];
	
	struct snbuffer buf;
	
	snbuffer_setup(&buf, STDOUT_FILENO, buffer, BUFFER_LENGTH);
	
	// Build header, which includes a "parent directory" link if the selector indicated a subdirectory
	snbuffer_push(&buf, BUFFER_BUFFER, snprintf(buf.pos, buf.size, "iDirectory listing of %s%s\r\n", env_hostname, selector));
	
	if (n > 0)
	{
		char* slash = selector;
		
		for (int i = 0; i < n; i++)
		{
			slash = strchr(slash, '/') + 1;
		}
		
		snbuffer_push(&buf, BUFFER_BUFFER, snprintf(buf.pos, buf.size, "1Parent Directory\t%.*s\t%s\t%s\r\n", (int)(slash - selector), selector, env_hostname, env_port));
	}
	
	snbuffer_push(&buf, BUFFER_BUFFER, snprintf(buf.pos, buf.size, "i\r\n"));
	
	// Build list of filenames
	for (size_t i = 0; i < filenames_count; i++)
	{
		char* filename = filenames[i];
		
		// Get the file stats
		struct stat statbuf;
		
		if (stat(filename, &statbuf) < 0)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot stat %s: %s\n", getpid(), filename, strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		// Make sure it's world readable
		if (!(statbuf.st_mode & S_IROTH))
		{
			continue;
		}
		
		char type;
		
		// Make sure it's a regular file, or a directory that is world executable
		if (S_ISREG(statbuf.st_mode))
		{
			// Determine if it's executable or not
			if (statbuf.st_mode & S_IXOTH)
			{
				// Treat executables as a query menu
				// This is probably as good a guess as any, but if it's meant to be downloaded it shouldn't be +x
				type = '7';
			}
			else
			{
				// Default behavior for regular non-executable files is to download as a binary file
				type = '9';
				
				// If the file has an extension, inspect it for further context
				char* extension = strrchr(filename, '.');
				
				if (extension != NULL)
				{
					// Advance past the period
					extension++;
					
					// Search the table for a match
					struct ext_entry key =
					{
						.ext = extension
					};
					
					const struct ext_entry* found = bsearch(&key, ext_table, sizeof(ext_table)/sizeof(struct ext_entry), sizeof(struct ext_entry), compare_ext_entry);
					
					if (found != NULL)
					{
						type = found->type;
					}
				}
			}
		}
		else if (S_ISDIR(statbuf.st_mode) && statbuf.st_mode & S_IXOTH)
		{
			// Treat directories as being submenus
			// You did put a gophermap in it, right?
			type = '1';
		}
		else
		{
			continue;
		}
		
		// Build the menu line
		snbuffer_push(&buf, BUFFER_BUFFER, snprintf(buf.pos, buf.size, "%c%s\t%s%s\t%s\t%s\r\n", type, filename, selector, filename, env_hostname, env_port));
	}
	
	// Add the footer and output the buffer
	snbuffer_push(&buf, 0, snprintf(buf.pos, buf.size, ".\r\n"));
	
	snbuffer_flush(&buf);
	
	exit(EXIT_SUCCESS);
}
