CC=clang

CFLAGS=-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror

DEBUG_FLAGS=-O0 -g -fsanitize=address -fsanitize=undefined

SRC_DIR=src
BUILD_DIR=build

SRCS=$(wildcard $(SRC_DIR)/*.c)
HS=$(wildcard $(SRC_DIR)/*.h)
OBJS=$(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

ifeq ($(OS),Windows_NT)
	TARGET=sws.exe
	LDLIBS=-lws2_32
else
	TARGET=sws
	CFLAGS+=-pthread
	LDLIBS=-pthread
endif

MODEL?=event
PORT?=4545
ROOT?=./www

.PHONY: all run debug format lint quality clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET) --model $(MODEL) --port $(PORT) --root $(ROOT)

debug:
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)

format:
	clang-format -i $(SRCS) $(HS)

lint:
	clang-tidy $(SRCS) -- $(CFLAGS)

quality: format lint

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(BUILD_DIR)
	rm -rf $(TARGET).dSYM
	rm -rf .cache
