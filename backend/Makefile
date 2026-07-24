# Builds hsedd — the privileged backend daemon. Naming split is deliberate:
#   hsedd  = the daemon (this binary) — parses /proc, does ftruncate/kill/
#            ptrace, speaks the socket protocol. Lives in /usr/sbin, the
#            same convention as sshd/nginx for unattended, privileged
#            services.
#   hsed   = the interactive TUI command a user actually types — that's
#            the Python package in ../hidden_space_explorer, installed as
#            /usr/bin/hsed by the .deb (see ../packaging/).
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -std=gnu11
LDFLAGS ?= -pthread

SRC_DIR := src
BUILD_DIR := build
BIN := hsedd

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN)

# Installs the binary + a systemd unit. Override PREFIX/UNITDIR as needed.
PREFIX  ?= /usr
UNITDIR ?= /etc/systemd/system

install: $(BIN)
	install -Dm755 $(BIN) $(PREFIX)/sbin/$(BIN)
	install -Dm644 systemd/hsed.service $(UNITDIR)/hsed.service
	@echo "Installed. Run: systemctl daemon-reload && systemctl enable --now hsed"
