CC = gcc
CFLAGS = -D_FORTIFY_SOURCE=2 -Werror -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wconversion -O3
LDFLAGS = 

sgopher_OBJFILES = main.o server.o sepoll.o sfork.o
gophertester_OBJFILES = gophertester.o smalloc.o
gopherlist_OBJFILES = gopherlist.o

OBJFILES = $(sgopher_OBJFILES) $(gophertester_OBJFILES) $(gopherlist_OBJFILES)
TARGETS = sgopher gophertester gopherlist

all: $(TARGETS)

sgopher: $(sgopher_OBJFILES)
	$(CC) $(CFLAGS) -o $@ $(sgopher_OBJFILES) $(LDFLAGS)

gophertester: $(gophertester_OBJFILES)
	$(CC) $(CFLAGS) -o $@ $(gophertester_OBJFILES) $(LDFLAGS)

gopherlist: $(gopherlist_OBJFILES)
	$(CC) $(CFLAGS) -o $@ $(gopherlist_OBJFILES) $(LDFLAGS)

clean:
	rm -f $(OBJFILES) $(TARGETS)
