CC=clang

CFLAGS=-Wall -Wextra -Wpedantic -Werror

DEBUG_FLAGS=-g -fsanitize=address -fsanitize=undefined

TARGET=sws

SRCS=$(wildcard *.c)

OBJS=$(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)
	./${TARGET}

%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(TARGET).dSYM