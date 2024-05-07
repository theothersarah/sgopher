// for twalk_r, tdestroy, and O_PATH
#define _GNU_SOURCE

// opendir, readdir, closedir
#include <dirent.h>

// errno
#include <errno.h>

// open
#include <fcntl.h>

// tsearch, twalk_r, tdestroy
#include <search.h>

// snprintf, fprintf
#include <stdio.h>

// getenv, malloc, free
#include <stdlib.h>

// strerror, strcpy, strchr, strlen, strncat, strcat, strrchr, strcmp
#include <string.h>

// fstat
#include <sys/stat.h>

// write, close, getpid
#include <unistd.h>

// Buffering for several snprintf calls to limit the number of writes performed
#define BUFFERSIZE 65536
#define BUFFER_BUFFER 1024

char buffer[BUFFERSIZE];
char* buffer_pos = buffer;
size_t buffer_cnt = 0;
size_t buffer_rem = BUFFERSIZE;

// If there's anything in the buffer, write it out
void buffer_flush()
{
	if (buffer_cnt > 0)
	{
		do
		{
			ssize_t cnt = write(STDOUT_FILENO, buffer, buffer_cnt);
			
			if (cnt < 0)
			{
				fprintf(stderr, "%i (gopherlist) - Error: Cannot write: %s\n", getpid(), strerror(errno));
				exit(EXIT_FAILURE);
			}
			
			buffer_cnt -= (size_t)cnt;
		}
		while(buffer_cnt > 0);
		
		buffer_pos = buffer;
		buffer_cnt = 0;
		buffer_rem = BUFFERSIZE;
	}
}

// Acknowledge the results of the snprintf call and update position and count, and flush the buffer if it gets too full
void buffer_push(int size)
{
	if (size < 0)
	{
		return;
	}
	else if ((size_t)size > buffer_rem)
	{
		return;
	}
	
	buffer_pos += size;
	buffer_cnt += (size_t)size;
	buffer_rem -= (size_t)size;
	
	if (buffer_rem < BUFFER_BUFFER)
	{
		buffer_flush();
	}
}

// Arguments passed to filename processing
struct tree_args
{
	const char* selector;
	const char* hostname;
	const char* port;
};

// Mapping of extension to selector type
// Not comprehensive but at least an assortment of common and period-accurate stuff
struct ext_entry
{
	char* ext;
	char type;
};

struct ext_entry ext_table[] =
{
	{"txt", '0'},
	{"c", '0'},
	{"h", '0'},
	{"gif", 'g'},
	{"jpg", 'I'},
	{"jpeg", 'I'},
	{"png", 'I'},
	{"bmp", 'I'},
	{"tif", 'I'},
	{"tiff", 'I'},
	{"pcx", 'I'},
	{NULL, '\0'}
};

// This is called by the tree traversal to handle each filename that had been placed in the tree
void process_filename(const void* node, VISIT which, void* closure)
{
	// Want to handle them in sort order
	if (which == preorder || which == endorder)
	{
		return;
	}
	
	// Convert arguments into useful variables
	const char* filename = *(char**)node;
	struct tree_args* args = (struct tree_args*)closure;
	
	// Now we have to open files to stat them
	int file = open(filename, O_RDONLY | O_PATH);
	
	if (file < 0)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot open %s : %s\n", getpid(), filename, strerror(errno));
		return;
	}
	
	struct stat statbuf;
	
	if (fstat(file, &statbuf) < 0)
	{
		fprintf(stderr, "%i (gopherlist) - Error: Cannot fstat %s: %s\n", getpid(), filename, strerror(errno));
		close(file);
		return;
	}
	
	close(file);
	
	// Make sure it's world readable
	if (!(statbuf.st_mode & S_IROTH))
	{
		return;
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
				extension++;
				
				struct ext_entry* curr_ext = ext_table;
				
				while (curr_ext->ext != NULL)
				{
					if (strcmp(extension, curr_ext->ext) == 0)
					{
						type = curr_ext->type;
						break;
					}
					
					curr_ext++;
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
		return;
	}
	
	// Build the menu line
	buffer_push(snprintf(buffer_pos, buffer_rem, "%c%s\t%s%s\t%s\t%s\r\n", type, filename, args->selector, filename, args->hostname, args->port));
}

// Tree search string compare
static int compare_strings(const void* pa, const void* pb)
{
	return (strcmp(pa, pb));
}

// Main function
int main()
{
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
	
	// This is the root of a tree that contains all the accepted filenames
	void* filenames = NULL;
	
	// Open a directory stream for the working directory so we can go through its files and add them to the tree
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
		
		// We can't rely on the string existing over multiple iterations if the directory is large
		// So allocate memory and copy it
		char* filename = malloc(strlen(entry->d_name) + 1);
		
		if (filename == NULL)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for filename: %s\n", getpid(), strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		strcpy(filename, entry->d_name);
		
		// Add the filename to the tree
		if (tsearch(filename, &filenames, compare_strings) == NULL)
		{
			fprintf(stderr, "%i (gopherlist) - Error: Cannot allocate memory for tree node\n", getpid());
			exit(EXIT_FAILURE);
		}
	}
	
	closedir(directory);
	
	// Build header, which includes a "parent directory" link if the selector indicated a subdirectory
	buffer_push(snprintf(buffer_pos, buffer_rem, "iDirectory listing of %s%s\r\n", env_hostname, selector));
	
	if (n > 0)
	{
		char* slash = selector;
		
		for (int i = 0; i < n; i++)
		{
			slash = strchr(slash, '/') + 1;
		}
		
		buffer_push(snprintf(buffer_pos, buffer_rem, "1Parent Directory\t%.*s\t%s\t%s\r\n", (int)(slash - selector), selector, env_hostname, env_port));
	}
	
	buffer_push(snprintf(buffer_pos, buffer_rem, "i\r\n"));
	
	// Traverse the tree to add menu lines to the buffer and then destroy it
	struct tree_args args =
	{
		.selector = selector,
		.hostname = env_hostname,
		.port = env_port
	};
	
	twalk_r(filenames, process_filename, &args);
	
	tdestroy(filenames, free);
	
	// Add the footer
	buffer_push(snprintf(buffer_pos, buffer_rem, ".\r\n"));
	
	// Output anything remaining
	buffer_flush();
	
	exit(EXIT_SUCCESS);
}
