CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -std=gnu11
LDFLAGS ?= -pthread

SRC_DIR := src
BUILD_DIR := build
BIN := hsedd

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean install arm64

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN) $(BIN)-arm64 build-arm64


arm64:
	$(MAKE) CC=aarch64-linux-gnu-gcc BUILD_DIR=build-arm64 BIN=$(BIN)-arm64 all

PREFIX  ?= /usr
UNITDIR ?= /etc/systemd/system

install: $(BIN)
	install -Dm755 $(BIN) $(PREFIX)/sbin/$(BIN)
	install -Dm644 systemd/hsed.service $(UNITDIR)/hsed.service
	@echo "Installed. Run: systemctl daemon-reload && systemctl enable --now hsed"
