CC = gcc
CFLAGS = -Werror -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wconversion -Ofast
LDFLAGS = 
OBJFILES = main.o server.o sepoll.o sfork.o
TARGET = sgopher

all: $(TARGET)

$(TARGET): $(OBJFILES)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJFILES) $(LDFLAGS)

clean:
	rm -f $(OBJFILES) $(TARGET)
