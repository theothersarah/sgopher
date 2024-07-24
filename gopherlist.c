// for memrchr, strcasestr, strdupa
#define _GNU_SOURCE

// opendir, readdir, closedir
#include <dirent.h>

// errno
#include <errno.h>

// snprintf, fprintf
#include <stdio.h>

// on_exit, exit, getenv, calloc, reallocarray, free, qsort, bsearch
#include <stdlib.h>

// strerror, strdupa, strlen, strrchr, strcmp, memrchr, strcasestr
#include <string.h>

// stat
#include <sys/stat.h>

// write, getpid
#include <unistd.h>

// Starting size of filename string list
#define NUM_FILENAMES 256

// Buffering for several snprintf calls to limit the number of writes performed
#define BUFFER_LENGTH 65536
#define BUFFER_BUFFER 4096

struct snbuffer_t
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

static inline void snbuffer_setup(struct snbuffer_t* snbuffer, int fd, char* base, size_t len)
{
	snbuffer->fd = fd;
	
	snbuffer->base = base;
	snbuffer->len = len;
	
	snbuffer->pos = base;
	snbuffer->size = len;
}

void snbuffer_flush(struct snbuffer_t* snbuffer)
{
	size_t count = (size_t)(snbuffer->pos - snbuffer->base);
	
	if (count > 0)
	{
		// Potentially requires multiple writes especially if the FD is a socket
		char* from = snbuffer->base;
		
		do
		{
			ssize_t n = write(snbuffer->fd, from, count);
			
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
		snbuffer->pos = snbuffer->base;
		snbuffer->size = snbuffer->len;
	}
}

void snbuffer_push(struct snbuffer_t* snbuffer, size_t leftover, int size)
{
	if (size > 0)
	{
		// >= is used to take the implied null terminator into account: if size == buf->size, one character is actually cut off in favor of the null
		if ((size_t)size >= snbuffer->size)
		{
			fprintf(stderr, "%i (gopherlist) - Warning: Discarded snprintf result due to size\n", getpid());
		}
		else
		{
			// Update position and remaining size for next call
			snbuffer->pos += size;
			snbuffer->size -= (size_t)size;
		}
	}
	else if (size < 0)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot snprintf: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Check if a flush is necessary to free space for the next call
	if (snbuffer->size < leftover)
	{
		snbuffer_flush(snbuffer);
	}
}

// List of filenames
struct filenamelist_t
{
	// Array of filenames
	char** filenames;
	
	// Size of array
	size_t size;
	
	// Number of filenames present in array
	size_t count;
};

// on_exit cleanup function for malloc with support for realloc
// Takes a pointer to the pointer since realloc may change the value of it
static void cleanup_realloc(int code, void* arg)
{
	void** ptr = arg;
	
	free(*ptr);
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

struct ext_entry_t
{
	const char* ext;
	const char type;
};

static const struct ext_entry_t ext_table[] =
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
	const struct ext_entry_t* entry1 = pa;
	const struct ext_entry_t* entry2 = pb;
	
	return strcmp(entry1->ext, entry2->ext);
}

// Main function
int main()
{
	// List of filenames which must be freed on exit
	struct filenamelist_t filenamelist =
	{
		.filenames = NULL,
		.size = 0,
		.count = 0
	};
	
	on_exit(cleanup_realloc, &filenamelist.filenames);
	
	// Get the key environment variables we need
	// It's fine if they are null, too, although it would generate a non-functional menu
	char* env_selector = getenv("SCRIPT_NAME");
	char* env_hostname = getenv("SERVER_NAME");
	char* env_port = getenv("SERVER_PORT");
	char* env_query = getenv("QUERY_STRING");
	
	// Examine selector for key positions of slashes
	// Figure out where the final slash is, assuming it denotes what the containing directory is
	char* last_slash = NULL;
	
	if (env_selector != NULL)
	{
		last_slash = strrchr(env_selector, '/');
	}
	
	// Find out where the second last slash is, assuming it denotes what the parent directory is
	char* parent_slash = NULL;
	
	if (last_slash != NULL)
	{
		parent_slash = memrchr(env_selector, '/', (size_t)(last_slash - env_selector));
	}
	else
	{
		// So that last_slash - env_selector = 0
		last_slash = env_selector;
	}
	
	// Figure out if there was actually a query by checking it for a non-zero length
	size_t query_len = 0;
	
	if (env_query != NULL)
	{
		query_len = strlen(env_query);
	}
	
	// Allocate a list of filenames with a starter size that may be expanded later if needed
	filenamelist.size = NUM_FILENAMES;
	
	filenamelist.filenames = calloc(filenamelist.size, sizeof(char*));
	
	if (filenamelist.filenames == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename list: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Open a directory stream for the working directory
	DIR* directory = opendir(".");
	
	if (directory == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot opendir: %s\n", getpid(), strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	// Go through the files and add them to the list if they meet some basic requirements
	struct dirent* entry;
	
	while (entry = readdir(directory), entry != NULL)
	{
		// Ignore hidden files
		if (entry->d_name[0] == '.')
		{
			continue;
		}
		
		// Check filename for query string, case-insensitive, and discard it if there was no match
		if (query_len > 0)
		{
			if (strcasestr(entry->d_name, env_query) == NULL)
			{
				continue;
			}
		}
		
		// Make a copy of the filename
		char* filename = strdupa(entry->d_name);
		
		if (filename == NULL)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename: %s\n", getpid(), strerror(errno));
			closedir(directory);
			exit(EXIT_FAILURE);
		}
		
		// Add the filename to the list
		filenamelist.filenames[filenamelist.count++] = filename;
		
		// Resize the filename list if it's full, doubling it in size
		if (filenamelist.count == filenamelist.size)
		{
			filenamelist.size *= 2;
			
			filenamelist.filenames = reallocarray(filenamelist.filenames, filenamelist.size, sizeof(char*));
			
			if (filenamelist.filenames == NULL)
			{
				fprintf(stderr, "%i (gopherlist) - Error: Cannot reallocate memory for filenames: %s\n", getpid(), strerror(errno));
				closedir(directory);
				exit(EXIT_FAILURE);
			}
		}
	}
	
	closedir(directory);
	
	// Sort the list of filenames
	qsort(filenamelist.filenames, filenamelist.count, sizeof(char*), compare_strings);
	
	// Prepare a buffer for the output to produce a minimum number of writes
	struct snbuffer_t snbuffer;
	char buffer[BUFFER_LENGTH];
	
	snbuffer_setup(&snbuffer, STDOUT_FILENO, buffer, BUFFER_LENGTH);
	
	// Build header, which includes a "parent directory" link if one can be derived from the selector
	snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "iDirectory listing of %s:%s%.*s/\r\n", env_hostname, env_port, (int)(last_slash - env_selector), env_selector));
	
	if (query_len > 0)
	{
		snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "iShowing filenames containing %s\r\n", env_query));
	}
	
	snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "i\r\n"));
	
	if (parent_slash != NULL)
	{
		snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "1Parent Directory\t%.*s\t%s\t%s\r\n", (int)(parent_slash - env_selector) + 1, env_selector, env_hostname, env_port));
	}
	
	// Counter for valid files
	unsigned int files_found = 0;
	
	// Build list of filenames
	for (size_t i = 0; i < filenamelist.count; i++)
	{
		char* filename = filenamelist.filenames[i];
		
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
				// Default behavior for regular non-executable files with no or an unknown extension is to download as a binary file
				type = '9';
				
				// If the file has an extension, inspect it for further context
				char* extension = strrchr(filename, '.');
				
				if (extension != NULL)
				{
					// Advance past the period
					extension++;
					
					// Search the table for a match
					const struct ext_entry_t key =
					{
						.ext = extension
					};
					
					const struct ext_entry_t* found = bsearch(&key, ext_table, sizeof(ext_table)/sizeof(struct ext_entry_t), sizeof(struct ext_entry_t), compare_ext_entry);
					
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
			// Skip other types of files
			continue;
		}
		
		files_found++;
		
		// Build the menu line
		snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "%c%s\t%.*s/%s\t%s\t%s\r\n", type, filename, (int)(last_slash - env_selector), env_selector, filename, env_hostname, env_port));
	}
	
	// Add the footer and output the buffer
	if (query_len > 0)
	{
		snbuffer_push(&snbuffer, BUFFER_BUFFER, snprintf(snbuffer.pos, snbuffer.size, "i\r\niFound %u files\r\n", files_found));
	}
	
	snbuffer_push(&snbuffer, 0, snprintf(snbuffer.pos, snbuffer.size, ".\r\n"));
	
	snbuffer_flush(&snbuffer);
	
	exit(EXIT_SUCCESS);
}
