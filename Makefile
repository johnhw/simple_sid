CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS = -lm

# Source files
SRCS = simple_sid.c sid_test.c

# Object files
OBJS = $(SRCS:.c=.o)

# Executable
EXEC = sid

# Default target
all: $(EXEC)

# Link object files to create executable
$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target to remove object files and executable
clean:
	rm -f $(OBJS) $(EXEC)

.PHONY: all clean