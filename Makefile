SRC_DIR := src
BIN_DIR := build

CC     := gcc
CFLAGS := -Wall -Wextra -Wpedantic -std=c99

DEBUG ?= no
ifeq ($(DEBUG), yes)
	CFLAGS += -O0 -g
	CONFIG := debug
else
	CFLAGS += -O2
	CONFIG := release
endif

BUILD_DIR := $(BIN_DIR)/$(CONFIG)

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(addprefix $(BUILD_DIR), /$(SRCS:.c=.o))

TARGET := $(BUILD_DIR)/kilo

RM := rm -rf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(BIN_DIR)
