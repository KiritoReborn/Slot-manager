#ifndef MODELS_H
#define MODELS_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 9090
#define SERVER_BACKLOG 64

#define SHM_KEY 0x41550001
#define MSGQ_KEY 0x41550002
#define RATE_SEM_NAME "/placement_rate"

#define MAX_CONCURRENT_CONNECTIONS 50
#define MAX_COMPANIES 10
#define MAX_ROUNDS 5
#define MAX_SLOTS_PER_ROUND 20
#define MAX_STUDENTS 500
#define MAX_WAITLIST_ENTRIES 1024

#define USERS_FILE "data/users.txt"
#define AUDIT_LOG_FILE "data/placement_audit.log"
#define OUTCOMES_FILE "data/outcomes.bin"
#define STATE_FILE "data/placement_state.bin"

#define CHECKIN_DEADLINE_SECONDS 1800
#define WAITLIST_MAX_MTYPE 1000
#define WAITING_ROOM_CAPACITY 256

typedef enum {
    ROLE_ADMIN = 0,
    ROLE_HR = 1,
    ROLE_STUDENT = 2
} UserRole;

typedef enum {
    REQ_VIEW = 0,
    REQ_BOOK,
    REQ_CANCEL,
    REQ_CHECKIN,
    REQ_JOIN_WAITING_ROOM,
    REQ_HR_ADD_SLOT,
    REQ_HR_MARK_OUTCOME,
    REQ_HR_NEXT_CANDIDATE,
    REQ_HR_EXTEND_INTERVIEW,
    REQ_SWAP_PROPOSE,
    REQ_SWAP_ACCEPT,
    REQ_SWAP_REJECT,
    REQ_ADMIN_BAN,
    REQ_ADMIN_FORCE_CANCEL,
    REP_SUCCESS,
    REP_FAIL,
    REP_ALERT,
    REP_SERVER_FULL,
    REP_WAITLIST_PROMOTED,
    REP_SWAP_INCOMING
} MessageType;

typedef enum {
    OUTCOME_PENDING = 0,
    OUTCOME_PASS,
    OUTCOME_FAIL,
    OUTCOME_HOLD,
    OUTCOME_NO_SHOW
} InterviewOutcome;

typedef struct {
    MessageType type;
    int user_id;
    int company_id;
    int round_id;
    int slot_id;
    int target_student_id;
    char payload[256];
} NetworkPacket;

typedef struct {
    int student_id;
    int company_id;
    int round_id;
    int slot_id;
    timer_t timer_id;
} BookingContext;

typedef struct {
    int slot_id;
    char time_window[20];
    int is_booked;
    int checked_in;
    int booked_student_id;
    InterviewOutcome outcome;
    pthread_mutex_t slot_lock;
    BookingContext *booking_ctx;
    time_t booking_epoch;
} InterviewSlot;

typedef struct {
    int round_number;
    char round_name[32];
    int duration_minutes;
    InterviewSlot slots[MAX_SLOTS_PER_ROUND];
    int total_slots;
    int available_slots;
} InterviewRound;

typedef struct {
    int company_id;
    char company_name[50];
    InterviewRound rounds[MAX_ROUNDS];
    int total_rounds;
    pthread_rwlock_t company_lock;
} Company;

typedef struct {
    long priority;
    int student_id;
    int company_id;
    int round_id;
    int slot_id;
    float cgpa;
    char student_name[64];
} WaitlistEntry;

typedef struct {
    Company companies[MAX_COMPANIES];
    int active_student_connections;
    pthread_mutex_t shm_lock;
    pthread_mutexattr_t shm_mutexattr;
    WaitlistEntry waitlist_entries[MAX_WAITLIST_ENTRIES];
    int waitlist_count;
    pthread_mutex_t waitlist_lock;
} PlacementState;

typedef struct {
    InterviewOutcome results[MAX_STUDENTS][MAX_COMPANIES][MAX_ROUNDS];
    pthread_mutex_t outcome_lock;
} OutcomeTable;

struct waitlist_msgbuf {
    long mtype;
    int student_id;
    int requested_slot_id;
    int company_id;
    int round_id;
    float cgpa;
    char student_name[64];
};


typedef struct {
    int id;
    char username[64];
    char password[64];
    UserRole role;
    int company_id;
    float cgpa;
    int ban_count;
    int daily_bookings;
} UserRecord;

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
} ClientThreadArgs;

typedef struct {
    int shm_id;
    int msgq_id;
    int server_fd;
    int outcomes_fd;
    sem_t *rate_sem;
    PlacementState *state;
    OutcomeTable *outcome_table;
    volatile sig_atomic_t running;
} ServerContext;

// sets up shared stuff for server, makes it if u want
int ipc_initialize(ServerContext *ctx, int create_new);
// cleans up IPC stuff, nukes it if asked
void ipc_cleanup(ServerContext *ctx, int unlink_all);
// attaches to shared state (read-only)
PlacementState *ipc_attach_state_ro(int *out_shmid);
// detaches shm state ptr
int ipc_detach_state(void *state_ptr);
// opens+maps outcomes table file
int ipc_init_outcome_table(ServerContext *ctx);
// unmaps/closes outcomes table
void ipc_close_outcome_table(ServerContext *ctx);
// seeds default companies/rounds
void ipc_seed_default_data(PlacementState *state);
// reads rate sem and tells u active/capacity
int ipc_get_rate_sem_active(sem_t *sem, int *active, int *capacity);
// makes sure data files/dir exist
int ipc_ensure_data_files(void);
// writes current state snapshot to disk
int ipc_save_state(ServerContext *ctx);
// loads state snapshot from disk
int ipc_load_state(ServerContext *ctx);

// validates creds + role
int auth_validate_user(const char *username, const char *password, UserRole expected_role, UserRecord *out_user);
// loads user by id
int auth_lookup_user_by_id(int user_id, UserRecord *out_user);
// pulls cgpa + name for a student
int auth_lookup_cgpa(int student_id, float *out_cgpa, char *out_name, size_t out_name_len);
// sets ban count to a value
int auth_set_ban_count(int student_id, int ban_count);
// bumps no-show counter and returns new count
int auth_increment_no_show(int student_id, int *new_ban_count);
// tweaks daily booking count
int auth_update_daily_bookings(int student_id, int delta, int *new_value);
// checks if user is banned
int auth_is_banned(int student_id, int *is_banned);
// appends a line to audit log
int auth_append_audit(const char *event_name, const char *details);

// writes outcome for a student/company/round
int outcome_tracker_set(ServerContext *ctx, int student_id, int company_id, int round_id, InterviewOutcome outcome);
// reads outcome for a student/company/round
InterviewOutcome outcome_tracker_get(ServerContext *ctx, int student_id, int company_id, int round_id);
// async msync of outcomes table
int outcome_tracker_sync_async(ServerContext *ctx);
// full msync of outcomes table
int outcome_tracker_sync_full(ServerContext *ctx);

// inits per-student locks
int round_engine_init(void);
// tears down round engine locks
void round_engine_shutdown(void);
// checks if student can book next round
int round_engine_can_book(ServerContext *ctx, int student_id, int company_id, int round_id);
// stores outcome + audit log
int round_engine_mark_outcome(ServerContext *ctx, int student_id, int company_id, int round_id, InterviewOutcome outcome);

// books a slot or waitlists
int slot_manager_book(ServerContext *ctx, int student_id, const char *student_name, float cgpa,
                      int company_id, int round_id, int slot_id);
// cancels a booking (force if admin)
int slot_manager_cancel(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id, int force);
// marks check-in
int slot_manager_checkin(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id);
// pushes student into waitlist
int slot_manager_enqueue_waitlist(ServerContext *ctx, int student_id, const char *student_name, float cgpa,
                                  int company_id, int round_id, int slot_id);
// promotes next waitlist entry for slot
int slot_manager_promote_waitlist(ServerContext *ctx, int company_id, int round_id, int slot_id);
// atomically swaps two bookings
int slot_manager_atomic_swap(ServerContext *ctx,
                             int student_a, int company_a, int round_a, int slot_a,
                             int student_b, int company_b, int round_b, int slot_b);
// adds a new slot for a round
int slot_manager_add_slot(ServerContext *ctx, int company_id, int round_id, const char *time_window);
// clears bookings after outcome
int slot_manager_finalize_after_outcome(ServerContext *ctx,
                                        int student_id,
                                        int company_id,
                                        int round_id,
                                        InterviewOutcome outcome);

// init waiting room queues
int waiting_room_init(void);
// shutdown waiting room queues
void waiting_room_shutdown(void);
// wakes all waiting threads
void waiting_room_wake_all(void);
// join a waiting room queue
int waiting_room_join(int company_id, int student_id);
// pop next candidate if available
int waiting_room_try_next_candidate(int company_id, int *out_student_id);
// waits for next candidate (blocking)
int waiting_room_next_candidate(int company_id, int *out_student_id, volatile sig_atomic_t *running_flag);
// gets queue count for company
int waiting_room_get_count(int company_id, int *out_count);

// stores ctx for timer callbacks
void timer_manager_init(ServerContext *ctx);
// disarms all timers
void timer_manager_shutdown(ServerContext *ctx);
// arms check-in timeout
int timer_manager_arm(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id, int deadline_sec);
// disarms a slot timer
int timer_manager_disarm(ServerContext *ctx, int company_id, int round_id, int slot_id);

// sends a full packet over socket
int send_packet(int fd, const NetworkPacket *packet);
// receives a full packet over socket
int recv_packet(int fd, NetworkPacket *packet);

// role enum -> string
const char *role_to_string(UserRole role);
// string -> role enum
UserRole role_from_string(const char *text);
// outcome enum -> string
const char *outcome_to_string(InterviewOutcome outcome);
// string -> outcome enum
InterviewOutcome outcome_from_string(const char *text);

#endif