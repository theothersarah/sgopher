# sgopher
Gopher server for Linux

sgopher is my personal attempt at a Gopher protocol server that is capable of a high rate of throughput for many concurrent users. The package also contains gophertester, a benchmarker for Gopher servers that I made for testing purposes, and gopherlist, a CGI-like program that produces a directory listing.

## Configuration
sgopher is configured entirely with command line options. It accepts the following options:

-d, --directory=STRING     Location to serve files from (default ./gopherroot)  
-h, --hostname=STRING      Externally-accessible hostname of server, used for generation of gophermaps (default localhost)  
-i, --indexfile=STRING     Default file to serve from a blank path or path referencing a directory (default .gophermap)  
-m, --maxclients=NUMBER    Maximum simultaneous clients per worker process (default 4096 clients)  
-p, --port=NUMBER          Network port (default port 70)  
-t, --timeout=NUMBER       Time in seconds before booting inactive client (default 10 seconds)  
-w, --workers=NUMBER       Number of worker processes (default 1 worker)

sgopher must be run as a user with permission to bind a socket to ports below 1024, or else a custom high port number must be used. It currently contains no provisions for access logging or throttling. Errors are reported via stderr.

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

## sgopher Technical Ramblings
sgopher makes use of Linux-specific features whereever that would reduce the number of system calls or allow the logic to be more simple. Examples include the use of accept4 in place of accept and a pair of setsockopt calls to obtain a non-blocking, close-on-exec client socket, or the use of a Linux-specific variant of fork that returns a pidfd instead of using separate fork and pidfd_open calls. It also uses more mundane Linux-isms like an epoll-based event loop with timerfds and signalfds mixed in with the sockets, and sendfile to transfer files without userspace buffers.

Version 5.4 or so of the Linux kernel is needed for sgopher to function, mainly due to pidfd functionality being added in the Linux 5 series.

In addition to having a fairly efficient epoll-based event loop with edge-triggered, non-blocking sockets that is limited by the speed of gigabit ethernet rather than CPU time in my test cases, sgopher can fork off additional worker proceses for even greater concurrency if you can find enough Gopher users on the entire planet for this to be necessary. It makes use of address and port reuse on the listening socket, allowing workers to be blissfully unaware of each other as the kernel loadbalances incoming connections between them. This also means that a bad actor listening on the same port could theoretically hijack some requests, but root privileges (or better yet, granting CAP_NET_BIND_SERVICE to sgopher so it can be run as an unprivileged user) are required for binding to Gopher's port 70 so this isn't likely to be an issue - or at least not be worse than a bad actor having root privilieges! Since the load balancing happens in the kernel and processes are only woken up if they are selected to handle a given client, there's no "thundering herd" problem, either.

As I am an amateur I make no security guarantees. However, wherever possible I attempted to prevent malformed or malicious requests from opening files outside of the serving directory. sgopher opens the serving directory and opens all files using openat relative to that directory after screening and processing incoming selector strings to ensure that the string contains no path elements that begin with a period and is in itself relative to the serving directory. This should prevent access to higher-level directories and also prevents access to hidden files, which is a side effect but not really an unwanted one.

Once a final path is obtained, the file is opened and its attributes inspected. It must be world readable and a regular file or a directory to be served. If it is a directory, it must be world-executable, and a further openat call is used to attempt to open a gophermap file within that directory. Once a final file is obtained, if it is not executable it is transferred to the client using sendfile as often as possible until it completes. The time function is used to obtain a simple timestamp for the last successful interaction with a client, which is used to periodically check for inactivity every time the server's timerfd ticks.

If the file is executable, the server forks, dup2s the client's socket over stdout, sets up CGI-like environment variables, and executes the file using fexecve. The server obtains a pidfd refering to this process and monitors it for completion. Because the server has no real idea of what the CGI program is doing, it can't check for timeout via timestamps as it can for regular file transfers. Instead, the server watches the socket attached to the process for inactivity and will disconnect the client if an outgoing message hasn't happened recently enough, indicating either a problem with the program or the client. This is achieved by periodically calling getsockopt on the client's socket with the TCP_INFO to get a tcp_info structure and checking the tcpi_last_data_sent field.

I originally made use of io_uring to batch epoll_ctl and close calls, but I wasn't certain how much that was really helping for the complexity it added so I removed it. Plus I figured that if I was going to use io_uring I may as well go all the way instead of using it for just a couple of relatively minor things. The old code can be seen in the two sepoll_batched files. I believe Linux version 5.10 or so is needed for all the io_uring functionality.

## Security Disclaimer
I currently cannot vouch that sgopher is completely secure, so expose it to the Internet at your own risk!
