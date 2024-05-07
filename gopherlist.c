// opendir, readdir, closedir
#include <dirent.h>

// open
#include <fcntl.h>

// printf, snprintf
#include <stdio.h>

// getenv
#include <stdlib.h>

// strchr, strlen, strncat, strcat, strrchr, strcmp
#include <string.h>

// fstat
#include <sys/stat.h>

// getcwd, close
#include <unistd.h>

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

void main()
{
	// Get the environment variables we need and only proceed if they all exist
	char* env_selector = getenv("SCRIPT_NAME");
	char* env_hostname = getenv("SERVER_NAME");
	char* env_port = getenv("SERVER_PORT");
	
	if (env_selector == NULL || env_hostname == NULL || env_port == NULL)
	{
		return;
	}
	
	// Get working directory path
	char cwd[PATH_MAX];
	getcwd(cwd, PATH_MAX);
	
	// Trim extra slashes out of the selector. Multiple slashes in a row are valid so this is mostly cosmetic.
	// Also makes sure the selector has a slash at the beginning and end which are also not strictly necessary,
	// but play nice with adding the hostname and filename to the front and end respectively.
	// Also makes it easier to generate the selector for the "up a level" link
	char selector[1024];
	selector[0] = '/';
	selector[1] = '\0';
	
	int n = 0;
	
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
	
	// Header, which includes a "parent directory" link if the selector indicated a subdirectory
	printf("iDirectory listing of %s%s\r\n", env_hostname, selector);
	
	if (n > 0)
	{
		char* slash = selector;
		
		for (int i = 0; i < n; i++)
		{
			slash = strchr(slash, '/') + 1;
		}
		
		printf("1Parent Directory\t%.*s\t%s\t%s\r\n", (int)(slash - selector), selector, env_hostname, env_port);
	}
	
	printf("i\r\n");
	
	// Open a directory stream for the working directory so we can go through its files
	DIR* directory = opendir(cwd);
	
	if (directory == NULL)
	{
		return;
	}
	
	struct dirent* entry;
	
	while (entry = readdir(directory), entry != NULL)
	{
		char* filename = entry->d_name;
		
		// Ignore hidden files
		if (filename[0] == '.')
		{
			continue;
		}
		
		// Now we have to open files to stat them
		int file = open(filename, O_RDONLY);
		
		if (file < 0)
		{
			continue;
		}
		
		struct stat statbuf;
		
		if (fstat(file, &statbuf) < 0)
		{
			close(file);
			continue;
		}
		
		close(file);
		
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
			continue;
		}
		
		// Build the selector string and output the menu line
		char fileSelector[1024];
		snprintf(fileSelector, 1024, "%s%s", selector, filename);
		
		printf("%c%s\t%s\t%s\t%s\r\n", type, filename, fileSelector, env_hostname, env_port);
	}
	
	closedir(directory);
	
	printf(".\r\n");
}
