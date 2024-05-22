# sgopher
Gopher server for Linux

sgopher is my personal attempt at a Gopher protocol server that is capable of a high rate of throughput for many concurrent users. The package also contains gophertester, a benchmarker for Gopher servers that I made for testing purposes, and gopherlist, a CGI-like program that produces a directory listing.

## Configuration
sgopher is configured entirely with command line options. It accepts the following options:

-d, --directory=STRING     Location to serve files from. Defaults to ./gopherroot  
-h, --hostname=STRING      Externally-accessible hostname of server for generation of gophermaps. Defaults to localhost  
-i, --indexfile=STRING     Default file to serve from a blank path or path referencing a directory. Defaults to .gophermap  
-m, --maxclients=NUMBER    Maximum simultaneous clients per worker process. Defaults to 4096  
-p, --port=NUMBER          Network port. Defaults to 70  
-t, --timeout=NUMBER       Time in seconds before booting inactive client. Defaults to 10  
-w, --workers=NUMBER       Number of worker processes. Defaults to 1

sgopher currently contains no provisions for logging or throttling. Errors are reported via stderr.

## Standards Support
sgopher supports the Gopher protocol as written in RFC 1436 with a small number of exceptions.

Firstly, it does not append a .CRLF sequence to the end of gophermaps. Static gophermaps must include this in the file to conform to the standard. Gophermaps generated dynamically by CGI must also do so.

Secondly, it transfers text files as they exist on the disk. It does not reprocess them to change line endings, escape periods at the start of a line, or anything else. It did so during testing and in my experience either clients don't care or they get confused by it, so I went with the simplest case in the end.

Note: sgopher insists that valid requests to end with a CRLF sequence as stated in RFC 1436. It will reject clients that only send LF.

## "CGI"
Executable files, scripts or binaries, are not served directly by sgopher. They are executed in a forked process similar to HTTP's CGI standard.

CGI programs are executed with the following file descriptors:

0 (stdin): /dev/null  
1 (stdout): client socket  
2 (stderr): The stderr of the server process, for error logging

Additionally, there will be an open file descriptor referring to the executable file itself. This is present due to a limitation of the fexecve function - if the executable file is a script with a shebang line, it will only execute correctly if the file is NOT marked CLOEXEC, so it persists.

The following environmental variables are provided, mimicking some of the CGI standard:

SCRIPT_NAME - the selector that was provided and resulted in the execution of this file.  
QUERY_STRING - if the selector string included a query separated from the selector string by a tab, as per Gopher's menu type 7, it will be present in this variable.  
SERVER_NAME - hostname of the server as provided via a command line option.  
SERVER_PORT - port of the server.  
REMOTE_ADDR - client's address.

See the gopherlist source for an example of some of this functionality.

Note: This means that if you wish to serve executable files for download, be sure to chmod -x them so sgopher does not try to execute them! I'm not sure what would necessarily happen if it did, but it can't be desirable.

## gophertester
This benchmark tool hammers a Gopher server as fast as it can, using one concurrent request per worker, with a provided request string. It runs for a set duration and keeps statistics for total requests, successful requests, timeouts, and size mismatches. The first time it encounters a timeout or size mismatch it will report it to the console.

It takes the following command line options:

-a, --address=STRING       Address of echo server (default 127.0.0.1)  
-d, --duration=NUMBER      Duration of test in seconds (default 60)  
-p, --port=NUMBER          Network port to use (default 8080)  
-r, --request=STRING       Request string (default /)  
-s, --size=NUMBER          Expected size of response in bytes (default 0 - do not check size)  
-t, --timeout=NUMBER       Time to wait for socket state change before giving up in milliseconds (default 1000)  
-w, --workers=NUMBER       Number of worker processes (default 1)

Note that it automatically appends CRLF to the request string, so you do not need to include that in the string passed to the --request option. For best results, use a large number of workers to maximize concurrent requests.

(Please, for the love of God, only use this on your own servers!)

## gopherlist
gopherlist is intended to be executed by the server itself to produce a directory listing, rather than building that functionality into the server itself. To use, make a symlink to it from any directory in which a listing is desired. Give the symlink the same name as the gophermap file (default .gophermap).

It should look something like this in a directory listing, and gopherlist itself should be executable:

lrwxrwxrwx 1 sarah sarah   16 May  6 06:49 .gophermap -> ../../gopherlist

When the directory is accessed by a user, a gophermap presenting a file listing will be generated and prsented to the client. Files will only be listed if they meet the requirements to be served: the filename must not start with a period, it must be world-readable, and if it is a directory it must also be world-executable. A parent directory link will be generated if applicable. The hostname must be set correctly with the --hostname option.

Note that gopherlist uses a simplistic method of generating menu selectors that is only valid if the selector sent to it had originally referred to the containing directory. This is fine with the default .gophermap name, because files starting with periods cannot be accessed directly, but be aware if you use a different name for your gophermaps.

## Brief sgopher Technical Background
sgopher makes use of Linux-specific features whereever that would reduce the number of system calls or allow the logic to be more simple. Examples include the use of accept4 in place of accept and a pair of setsockopt calls to optain a non-blocking, close-on-exec client socket, or the use of a Linux-specific variant of fork that returns a pidfd instead of using separate fork and pidfd_open calls. It also uses more mundane Linux-isms like an epoll-based event loop with timerfds and signalfds mixed in with the sockets, and sendfile to transfer files without userspace buffers.

In addition to having an efficient epoll-based event loop that is limited by gigabit ethernet in my test cases, sgopher can fork off additional worker proceses for greater concurrency. It makes use of address and port reuse on the listening socket, allowing workers to be blissfully unaware of each other as the kernel loadbalances incoming connections between them. This also means that a bad actor listening on the same port could theoretically hijack some requests, but root privileges are required for Gopher's port 70 so this isn't likely to be an issue - or at least not be worse than a bad actor having root privilieges!

As I am an amateur I make no security guarantees. However, wherever possible I attempted to prevent malformed or malicious requests from opening files outside of the serving directory. sgopher opens the serving directory and opens all files using openat relative to that directory after screening and processing incoming selector strings to ensure that the string contains no path elements that begin with a period and is in itself relative to the serving directory. This should prevent access to higher-level directories and also prevents access to hidden files, which is a side effect but not really an unwanted one.

Once a final path is obtained, the file is opened and its attributes inspected. It must be world readable and a regular file or a directory to be served. If it is a directory, it must be world-executable, and a further openat call is used to attempt to open a gophermap file within that directory. Once a final file is obtained, if it is not executable it is transferred to the client using sendfile as often as possible until it completes. If it is executable, the server forks, dup2s the client's socket over stdout, sets up CGI-like environment variables, and executes the file using fexecve. The server obtains a pidfd refering to this process and monitors it for completion. The server also watches the socket attached to the process for inactivity and will disconnect the client if an outgoing message hasn't happened recently enough.

I originally made use of io_uring to batch epoll_ctl and close calls, but I wasn't certain how much that was really helping for the complexity it added so I removed it. The old code can be seen in sepoll_batched.c.

## Security Disclaimer
I currently cannot vouch that sgopher is completely secure, so expose it to the Internet at your own risk!
