CC=clang

CFLAGS=-Wall -Wextra -Wpedantic -Werror

DEBUG_FLAGS=-g -fsanitize=address -fsanitize=undefined

TARGET=sws

SRCS=$(wildcard *.c)
HS=${wildcard *.h}

OBJS=$(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)
	./${TARGET}

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRCS) -o $(TARGET)

format:
	clang-format -i ${SRCS} ${HS}

lint:
	clang-tidy ${SRCS}

quality: format lint

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(TARGET).dSYM
	rm -rf .cache