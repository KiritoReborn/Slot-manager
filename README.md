# Campus Placement Drive Orchestrator

A terminal-based campus placement drive management system written in C. The project demonstrates core Operating Systems concepts including concurrency, IPC, synchronization, file locking, real-time timers, and memory mapping.

## Features

- Multi-threaded server with TCP socket clients (student, HR, admin roles)
- Priority waitlist using System V message queues
- POSIX real-time timers for no-show auto-cancel
- POSIX semaphore rate limiting for concurrent connections
- Shared memory live state (`PlacementState`)
- Memory-mapped outcomes file (`outcomes.bin`)
- Waiting room FIFO queue for HR
- Persistent state save/load (`placement_state.bin`)
- Audit log with file locking

## Project Structure

```
placement_drive/
  Makefile
  bin/
  data/
  src/
    client/
    common/
    server/
    tools/
```

## Build

```bash
cd placement_drive
make clean
make
```

Binaries:
- `bin/server`
- `bin/client`

## Run

```bash
# Terminal 1
./bin/server

# Terminal 2
./bin/client
```

Run additional clients in other terminals as needed.

## Data Files

- `data/users.txt` - user credentials and roles
- `data/placement_audit.log` - audit trail
- `data/outcomes.bin` - mmap-backed outcomes table
- `data/placement_state.bin` - persisted shared state
