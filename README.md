# sgopher
Gopher server for Linux

sgopher is my personal attempt at a Gopher protocol server that is capable of a high rate of throughput for many concurrent users. The package also contains gophertester, a benchmarker for Gopher servers that I made for testing purposes, and gopherlist, a CGI-like program that produces a directory listing.

Requires Linux kernel 5.5 or greater and glibc.

## Configuration
sgopher is configured entirely with command line options. It accepts the following options:

-d, --directory=STRING     Location to serve files from (default ./gopherroot)  
-h, --hostname=STRING      Externally-accessible hostname of server, used for generation of gophermaps (default localhost)  
-i, --indexfile=STRING     Default file to serve from a blank path or path referencing a directory (default .gophermap)  
-m, --maxclients=NUMBER    Maximum simultaneous clients per worker process (default 1000 clients)  
-p, --port=NUMBER          Network port (default port 70)  
-t, --timeout=NUMBER       Time in seconds before booting inactive client (default 10 seconds)  
-w, --workers=NUMBER       Number of worker processes (default 1 worker)

sgopher currently contains no provisions for access logging or throttling. Errors are reported via stderr.

The default maximum number of clients per process is set possibly higher than the number of concurrent Gopher users across the whole world. The number is fairly arbitrary; no significant resources are consumed if this number is higher than necessary - only 12 bytes per potential client as part of the event system. Additionally at startup, sgopher worker processes request an increase to the maximum number of open file descriptors if necessary to accommodate the maximum number of clients; it requires potentially up to 4 additional file descriptors per connected client.

When SIGTERM is sent to sgopher's main process, it will send SIGTERM to each of its children and wait for them to exit before exiting itself. This is intended to be the correct way to gracefully terminate sgopher. However, terminating it via SIGKILL or SIGINT via ctrl+c in the console does not cause any issues.

## Standards Support
sgopher supports the Gopher protocol as written in RFC 1436 with a small number of exceptions.

Firstly, it does not append a .CRLF sequence to the end of gophermaps. Static gophermaps must include this in the file to conform to the standard. Gophermaps generated dynamically by CGI must also do so.

Secondly, it transfers text files as they exist on the disk. It does not reprocess them to change line endings, escape periods at the start of a line, or anything else. It did so during testing and in my experience either clients don't care or they get confused by it, so I went with the simplest case in the end.

Note: sgopher insists that valid requests end with a CRLF sequence as stated in RFC 1436. It will reject clients that only send LF.

## "CGI"
Executable files, scripts or binaries, are not served directly by sgopher. They are executed in a forked process using an interface inspired by HTTP's CGI standard. These programs will be hereafter referred to as CGI programs, but note that it does not fully conform to the real CGI standard.

CGI programs are executed with the following file descriptors:

0 (stdin): /dev/null  
1 (stdout): client socket. Technically is read/write, but a well-behaved client shouldn't be sending anything.  
2 (stderr): The stderr pipe of the server process, for error reporting. Will show up in the console or wherever else the server's stderr is going.

Additionally, there will be an open file descriptor referring to the executable file itself. It will have whatever number it had when the server opened it for execution. This is present due to a limitation of the fexecve function - if the executable file is a script with a shebang line, it will only execute correctly if the file is NOT marked CLOEXEC, so it persists across the fexecve call. It is opened read-only so you can't accidentally overwrite it, though. I did not consider this to be enough of a problem to justify examining the file for the shebang.

The following environmental variables are provided, mimicking some aspects of the CGI standard:

SCRIPT_NAME - the selector that was provided and resulted in the execution of this file. It will be processed to include a / at the start, a / at the end if the selector referred to a directory, and with all redundant slashes removed.  
QUERY_STRING - if the selector string included a query separated from the selector string by a tab, as per Gopher's menu type 7, it will be present in this variable.  
SERVER_NAME - hostname of the server as provided via the command line option.  
SERVER_PORT - port of the server.  
REMOTE_ADDR - client's address.

See the gopherlist source for an example of some of this functionality, since it is implemented as a CGI program.

Note: This means that if you wish to serve executable files for download, be sure to chmod -x them so sgopher does not try to execute them! Execution of programs not meant as CGI programs can't possibly be desirable.

## gophertester
This benchmark tool hammers a Gopher server as fast as it can, using one concurrent request per worker, with a provided request string. It runs for a set duration and keeps statistics for total requests, successful requests, timeouts, and size mismatches. The first time it encounters a timeout or size mismatch it will report it to the console.

It takes the following command line options:

-a, --address=STRING       Address of Gopher server (default 127.0.0.1)  
-b, --buffersize=NUMBER    Size of receive buffer to use in bytes (default 65536 bytes)  
-d, --duration=NUMBER      Duration of test in seconds (default 60 seconds)  
-p, --port=NUMBER          Network port to use (default port 70)  
-r, --request=STRING       Request string without trailing CRLF sequence (default /)  
-s, --size=NUMBER          Expected size of response in bytes, or 0 for no size check (default 0 bytes - do not check size)  
-t, --timeout=NUMBER       Time to wait for socket state change before giving up in milliseconds, or a negative number for no timeout (default 1000 milliseconds)  
-w, --workers=NUMBER       Number of worker processes (default 1 worker)

Note that it automatically appends CRLF to the request string, so you do not need to include that in the string passed to the --request option. For best results, use a large number of workers to maximize concurrent requests.

(Please, for the love of God, only use this on your own servers! By design, it has no throttling and will easily saturate gigabit ethernet on my modest test setup.)

## gopherlist
gopherlist is intended to be executed by the server itself to produce a directory listing, rather than building that functionality into the server itself. For typical usage, make a symlink to it from any directory in which a listing is desired. Give the symlink the same name as the gophermap file (default .gophermap).

It should look something like this in a directory listing, and gopherlist itself should be executable:

lrwxrwxrwx 1 sarah sarah   16 May  6 06:49 .gophermap -> ../../gopherlist

When the directory is accessed by a user, a gophermap presenting a file listing will be generated and presented to the client. Files will only be listed if they meet the requirements to be served: the filename must not start with a period, it must be world-readable, and if it is a directory it must also be world-executable. A parent directory link will be generated if applicable based on the selector. Files will be assigned a menu type based on if they are executable files (menu type 7), directories (menu type 1), or otherwise, a variety of possibilities based on the file extension. The server's externally-accessible hostname must be set correctly with the --hostname option or the links will not work correctly.

gopherlist does not necessarily need to be the default gophermap of a directory. Based on the position of the final slash in the selector, it will determine if it was invoked by name or by directory and generate the listing accordingly. If it is not used as a default gophermap with a filename beginning with a period, it will list itself with menu type 7 among the files in the directory.

If invoked with a query string, it will display only those files with names that contain the provided query as a substring.

## Security

In order to use the default port of 70 for the Gopher protocol, sgopher must be run as root (NOT recommended!) or the executable itself must be granted the capability to use ports below 1024. Use  

sudo setcap cap_net_bind_service=ep ./sgopher

to do this. Additionally, ensure that sgopher is not world-executable so that unauthorized users can't run copies of their own on the same port, which would "hijack" a portion of the incoming connections. This is because to support multi-process handling of incoming connections, the workker processes enable port and address reuse on the listening port. This causes the kernel to distribute incoming connections evenly between sgopher processes, including potential "rogue" processes.

sgopher inspects selector strings provided by clients and rejects the client if any path components begin with ., which is intended to prevent use of relative paths to escape the serving directory. It also has the side-effect of preventing access to hidden files. Default gophermaps are not subject to this restriction, and in fact are hidden files by default. Selectors may begin with a / or not, and in either case are rebuilt as a relative path to prevent access outside of the server directory.

The permissions of the user sgopher is run by are used to determine access to files. Files must be readable and directories must be searchable, or else sgopher returns a forbidden error to the client. Files to be executed as CGI must be world executable, not simply executable by the server process' user.

## Disclaimer
I currently cannot vouch that sgopher is completely secure, so expose it to the Internet at your own risk!
