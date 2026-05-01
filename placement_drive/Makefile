CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -lrt -g
SRCDIR  = src
COMMON  = $(SRCDIR)/common/ipc_config.c

all: server client

server:
	$(CC) $(CFLAGS) \
	  $(SRCDIR)/server/main_server.c \
	  $(SRCDIR)/server/slot_manager.c \
	  $(SRCDIR)/server/auth_system.c \
	  $(SRCDIR)/server/outcome_tracker.c \
	  $(SRCDIR)/server/timer_manager.c \
	  $(SRCDIR)/server/round_engine.c \
	  $(SRCDIR)/server/waiting_room.c \
	  $(COMMON) -o bin/server

client:
	$(CC) $(CFLAGS) $(SRCDIR)/client/client.c $(COMMON) -o bin/client

clean:
	rm -f bin/* data/server.pid
	ipcrm -a 2>/dev/null || true
	sem_unlink /placement_rate 2>/dev/null || true

.PHONY: all server client clean
