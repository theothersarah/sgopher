// for memrchr, strcasestr, strdupa
#define _GNU_SOURCE

// opendir, readdir
#include <dirent.h>

// fprintf
#include <stdio.h>

// getenv, calloc, reallocarray, qsort, bsearch, exit
#include <stdlib.h>

// strdupa, strlen, strrchr, strcmp, memrchr, strcasestr
#include <string.h>

// stat
#include <sys/stat.h>

// getpid
#include <unistd.h>

// write buffering functions
#include "sbuffer.h"

// Starting size of filename string list
#define NUM_FILENAMES 256

// Buffer length and autoflush threshold
#define BUFFER_LENGTH 65536
#define BUFFER_LEFTOVER 4096

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
	char** filenames = calloc(NUM_FILENAMES, sizeof(char*));
	
	if (filenames == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename list: %m\n", getpid());
		exit(EXIT_FAILURE);
	}
	
	size_t filenames_size = NUM_FILENAMES;
	size_t filenames_count = 0;
	
	// Open a directory stream for the working directory
	DIR* directory = opendir(".");
	
	if (directory == NULL)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot opendir: %m\n", getpid());
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
			fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename: %m\n", getpid());
			exit(EXIT_FAILURE);
		}
		
		// Add the filename to the list
		filenames[filenames_count++] = filename;
		
		// Resize the filename list if it's full, doubling it in size
		if (filenames_count == filenames_size)
		{
			filenames_size *= 2;
			
			void* resized = reallocarray(filenames, filenames_size, sizeof(char*));
			
			if (resized == NULL)
			{
				fprintf(stderr, "%i (gopherlist) - Error: Cannot reallocate memory for filenames: %m\n", getpid());
				exit(EXIT_FAILURE);
			}
			
			filenames = resized;
		}
	}
	
	// Sort the list of filenames
	qsort(filenames, filenames_count, sizeof(char*), compare_strings);
	
	// Prepare a buffer for the output to reduce the number of writes to socket that are needed
	char buffer[BUFFER_LENGTH];
	struct sbuffer_t sbuffer;
	
	sbuffer_init(&sbuffer, STDOUT_FILENO, -1, buffer, BUFFER_LENGTH);
	
	// Build header, which includes a "parent directory" link if one can be derived from the selector
	sbuffer_push(&sbuffer, "iDirectory listing of %s:%s%.*s/\r\n", env_hostname, env_port, (int)(last_slash - env_selector), env_selector);
	
	if (query_len > 0)
	{
		sbuffer_push(&sbuffer, "iShowing filenames containing %s\r\n", env_query);
	}
	
	sbuffer_push(&sbuffer, "i\r\n");
	
	if (parent_slash != NULL)
	{
		sbuffer_push(&sbuffer, "1Parent Directory\t%.*s\t%s\t%s\r\n", (int)(parent_slash - env_selector) + 1, env_selector, env_hostname, env_port);
	}
	
	// Counter for valid files
	unsigned int files_found = 0;
	
	// Build list of filenames
	for (size_t i = 0; i < filenames_count; i++)
	{
		char* filename = filenames[i];
		
		// Get the file stats
		struct stat statbuf;
		
		if (stat(filename, &statbuf) < 0)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot stat %s: %m\n", getpid(), filename);
			exit(EXIT_FAILURE);
		}
		
		char type;
		
		// Make sure it's a regular file, or a directory
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
		else if (S_ISDIR(statbuf.st_mode))
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
		sbuffer_push(&sbuffer, "%c%s\t%.*s/%s\t%s\t%s\r\n", type, filename, (int)(last_slash - env_selector), env_selector, filename, env_hostname, env_port);
		
		// Check if buffer is full enough to flush
		sbuffer_checkflush(&sbuffer, BUFFER_LEFTOVER);
	}
	
	// Add the footer and output the buffer
	if (query_len > 0)
	{
		sbuffer_push(&sbuffer, "i\r\niFound %u files\r\n", files_found);
	}
	
	sbuffer_push(&sbuffer, ".\r\n");
	
	sbuffer_flush(&sbuffer);
	
	exit(EXIT_SUCCESS);
}
