#include "models.h"

#include <arpa/inet.h>

typedef struct {
    int slot_id;
    char time_window[20];
    int is_booked;
    int checked_in;
    int booked_student_id;
    int outcome;
    time_t booking_epoch;
} PersistSlot;

typedef struct {
    int round_number;
    char round_name[32];
    int duration_minutes;
    PersistSlot slots[MAX_SLOTS_PER_ROUND];
    int total_slots;
    int available_slots;
} PersistRound;

typedef struct {
    int company_id;
    char company_name[50];
    PersistRound rounds[MAX_ROUNDS];
    int total_rounds;
} PersistCompany;

typedef struct {
    PersistCompany companies[MAX_COMPANIES];
    int active_student_connections;
    WaitlistEntry waitlist_entries[MAX_WAITLIST_ENTRIES];
    int waitlist_count;
} PersistPlacementState;

// writes full buffer to socket
static int write_full(int fd, const void *buffer, size_t size) {
    const char *ptr = (const char *)buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t n = send(fd, ptr + total, size - total, 0);
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

// reads exact size from socket
static int read_full(int fd, void *buffer, size_t size) {
    char *ptr = (char *)buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t n = recv(fd, ptr + total, size - total, 0);
        if (n == 0) {
            return 1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

// init process-shared mutex
static int init_mutex_pshared(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return -1;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    if (pthread_mutex_init(mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    pthread_mutexattr_destroy(&attr);
    return 0;
}

// init process-shared rwlock
static int init_rwlock_pshared(pthread_rwlock_t *rwlock) {
    pthread_rwlockattr_t attr;
    if (pthread_rwlockattr_init(&attr) != 0) {
        return -1;
    }
    if (pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_rwlockattr_destroy(&attr);
        return -1;
    }
    if (pthread_rwlock_init(rwlock, &attr) != 0) {
        pthread_rwlockattr_destroy(&attr);
        return -1;
    }
    pthread_rwlockattr_destroy(&attr);
    return 0;
}

// builds default slot time window
static void build_time_window(char *out, size_t out_size, int base_hour, int slot_idx) {
    int start_minutes_total = base_hour * 60 + slot_idx * 15;
    int end_minutes_total = start_minutes_total + 15;

    int sh = start_minutes_total / 60;
    int sm = start_minutes_total % 60;
    int eh = end_minutes_total / 60;
    int em = end_minutes_total % 60;

    snprintf(out, out_size, "%02d:%02d-%02d:%02d", sh, sm, eh, em);
}

// fills a round with default slots
static void init_round(InterviewRound *round, int round_number, const char *round_name, int duration, int base_hour) {
    int i;

    memset(round, 0, sizeof(*round));
    round->round_number = round_number;
    strncpy(round->round_name, round_name, sizeof(round->round_name) - 1);
    round->duration_minutes = duration;
    round->total_slots = 8;
    round->available_slots = round->total_slots;

    for (i = 0; i < round->total_slots; ++i) {
        InterviewSlot *slot = &round->slots[i];
        slot->slot_id = i;
        build_time_window(slot->time_window, sizeof(slot->time_window), base_hour, i);
        slot->is_booked = 0;
        slot->checked_in = 0;
        slot->booked_student_id = -1;
        slot->outcome = OUTCOME_PENDING;
        slot->booking_ctx = NULL;
        slot->booking_epoch = 0;
        init_mutex_pshared(&slot->slot_lock);
    }
}

// seeds company/round defaults in shm
void ipc_seed_default_data(PlacementState *state) {
    static const char *company_names[] = {
        "Google", "Microsoft", "Amazon", "Apple", "Meta",
        "Nvidia", "Adobe", "Oracle", "Salesforce", "Atlassian"
    };
    static const char *default_round_name = "Interview";
    static const int default_round_duration = 30;
    int i;

    memset(state, 0, sizeof(*state));

    pthread_mutexattr_init(&state->shm_mutexattr);
    pthread_mutexattr_setpshared(&state->shm_mutexattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&state->shm_lock, &state->shm_mutexattr);

    init_mutex_pshared(&state->waitlist_lock);
    state->waitlist_count = 0;
    state->active_student_connections = 0;

    for (i = 0; i < MAX_COMPANIES; ++i) {
        Company *company = &state->companies[i];
        company->company_id = i + 1;
        strncpy(company->company_name, company_names[i], sizeof(company->company_name) - 1);
        company->total_rounds = 1;
        init_rwlock_pshared(&company->company_lock);

        init_round(&company->rounds[0], 1, default_round_name, default_round_duration, 9);
    }
}

// makes sure data dir/files exist
int ipc_ensure_data_files(void) {
    struct stat st;

    if (stat("data", &st) != 0) {
        if (mkdir("data", 0775) != 0 && errno != EEXIST) {
            return -1;
        }
    }
    int audit_fd = open(AUDIT_LOG_FILE, O_CREAT | O_RDWR, 0666);
    if (audit_fd < 0) {
        return -1;
    }
    close(audit_fd);

    int users_fd = open(USERS_FILE, O_CREAT | O_RDWR, 0666);
    if (users_fd < 0) {
        return -1;
    }
    close(users_fd);

    int state_fd = open(STATE_FILE, O_CREAT | O_RDWR, 0666);
    if (state_fd < 0) {
        return -1;
    }
    close(state_fd);

    return 0;
}

// copies shm state into disk snapshot
static void capture_persist_state(const PlacementState *src, PersistPlacementState *dst) {
    memset(dst, 0, sizeof(*dst));

    dst->active_student_connections = src->active_student_connections;
    dst->waitlist_count = src->waitlist_count;
    if (dst->waitlist_count > MAX_WAITLIST_ENTRIES) {
        dst->waitlist_count = MAX_WAITLIST_ENTRIES;
    }
    for (int i = 0; i < dst->waitlist_count; ++i) {
        dst->waitlist_entries[i] = src->waitlist_entries[i];
    }

    for (int c = 0; c < MAX_COMPANIES; ++c) {
        const Company *sc = &src->companies[c];
        PersistCompany *dc = &dst->companies[c];
        dc->company_id = sc->company_id;
        strncpy(dc->company_name, sc->company_name, sizeof(dc->company_name) - 1);
        dc->total_rounds = sc->total_rounds;

        for (int r = 0; r < sc->total_rounds && r < MAX_ROUNDS; ++r) {
            const InterviewRound *sr = &sc->rounds[r];
            PersistRound *dr = &dc->rounds[r];
            dr->round_number = sr->round_number;
            strncpy(dr->round_name, sr->round_name, sizeof(dr->round_name) - 1);
            dr->duration_minutes = sr->duration_minutes;
            dr->total_slots = sr->total_slots;
            dr->available_slots = sr->available_slots;

            for (int s = 0; s < sr->total_slots && s < MAX_SLOTS_PER_ROUND; ++s) {
                const InterviewSlot *ss = &sr->slots[s];
                PersistSlot *ds = &dr->slots[s];
                ds->slot_id = ss->slot_id;
                strncpy(ds->time_window, ss->time_window, sizeof(ds->time_window) - 1);
                ds->is_booked = ss->is_booked;
                ds->checked_in = ss->checked_in;
                ds->booked_student_id = ss->booked_student_id;
                ds->outcome = (int)ss->outcome;
                ds->booking_epoch = ss->booking_epoch;
            }
        }
    }
}

// applies snapshot back into shm
static void apply_persist_state(PlacementState *dst, const PersistPlacementState *src) {
    dst->active_student_connections = src->active_student_connections;
    dst->waitlist_count = src->waitlist_count;
    if (dst->waitlist_count > MAX_WAITLIST_ENTRIES) {
        dst->waitlist_count = MAX_WAITLIST_ENTRIES;
    }
    for (int i = 0; i < dst->waitlist_count; ++i) {
        dst->waitlist_entries[i] = src->waitlist_entries[i];
    }

    for (int c = 0; c < MAX_COMPANIES; ++c) {
        Company *dc = &dst->companies[c];
        const PersistCompany *sc = &src->companies[c];

        dc->company_id = sc->company_id;
        strncpy(dc->company_name, sc->company_name, sizeof(dc->company_name) - 1);
        dc->total_rounds = sc->total_rounds;
        if (dc->total_rounds > MAX_ROUNDS) {
            dc->total_rounds = MAX_ROUNDS;
        }

        for (int r = 0; r < dc->total_rounds; ++r) {
            InterviewRound *dr = &dc->rounds[r];
            const PersistRound *sr = &sc->rounds[r];
            int old_total_slots = dr->total_slots;

            dr->round_number = sr->round_number;
            strncpy(dr->round_name, sr->round_name, sizeof(dr->round_name) - 1);
            dr->duration_minutes = sr->duration_minutes;
            dr->total_slots = sr->total_slots;
            if (dr->total_slots > MAX_SLOTS_PER_ROUND) {
                dr->total_slots = MAX_SLOTS_PER_ROUND;
            }
            dr->available_slots = sr->available_slots;

            for (int s = old_total_slots; s < dr->total_slots; ++s) {
                pthread_mutex_init(&dr->slots[s].slot_lock, &dst->shm_mutexattr);
            }

            for (int s = 0; s < dr->total_slots; ++s) {
                InterviewSlot *ds = &dr->slots[s];
                const PersistSlot *ss = &sr->slots[s];
                ds->slot_id = ss->slot_id;
                strncpy(ds->time_window, ss->time_window, sizeof(ds->time_window) - 1);
                ds->is_booked = ss->is_booked;
                ds->checked_in = ss->checked_in;
                ds->booked_student_id = ss->booked_student_id;
                ds->outcome = (InterviewOutcome)ss->outcome;
                ds->booking_epoch = ss->booking_epoch;
                ds->booking_ctx = NULL;
            }
        }
    }
}

// saves shm state to file
int ipc_save_state(ServerContext *ctx) {
    PersistPlacementState snapshot;
    int fd;

    if (!ctx || !ctx->state) {
        errno = EINVAL;
        return -1;
    }

    capture_persist_state(ctx->state, &snapshot);

    fd = open(STATE_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        return -1;
    }

    if (write(fd, &snapshot, sizeof(snapshot)) != (ssize_t)sizeof(snapshot)) {
        close(fd);
        return -1;
    }

    fsync(fd);
    close(fd);
    return 0;
}

// loads shm state from file
int ipc_load_state(ServerContext *ctx) {
    PersistPlacementState snapshot;
    int fd;
    ssize_t n;

    if (!ctx || !ctx->state) {
        errno = EINVAL;
        return -1;
    }

    fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    n = read(fd, &snapshot, sizeof(snapshot));
    close(fd);

    if (n != (ssize_t)sizeof(snapshot)) {
        errno = EIO;
        return -1;
    }

    apply_persist_state(ctx->state, &snapshot);
    return 0;
}

// opens/mmap outcomes table
int ipc_init_outcome_table(ServerContext *ctx) {
    struct stat st;
    int needs_init = 0;

    ctx->outcomes_fd = open(OUTCOMES_FILE, O_RDWR | O_CREAT, 0666);
    if (ctx->outcomes_fd < 0) {
        perror("open outcomes");
        return -1;
    }

    if (fstat(ctx->outcomes_fd, &st) != 0) {
        perror("fstat outcomes");
        close(ctx->outcomes_fd);
        ctx->outcomes_fd = -1;
        return -1;
    }

    if ((size_t)st.st_size < sizeof(OutcomeTable)) {
        if (ftruncate(ctx->outcomes_fd, (off_t)sizeof(OutcomeTable)) != 0) {
            perror("ftruncate outcomes");
            close(ctx->outcomes_fd);
            ctx->outcomes_fd = -1;
            return -1;
        }
        needs_init = 1;
    }

    ctx->outcome_table = mmap(NULL, sizeof(OutcomeTable), PROT_READ | PROT_WRITE, MAP_SHARED, ctx->outcomes_fd, 0);
    if (ctx->outcome_table == MAP_FAILED) {
        perror("mmap outcomes");
        close(ctx->outcomes_fd);
        ctx->outcomes_fd = -1;
        ctx->outcome_table = NULL;
        return -1;
    }

    if (needs_init) {
        pthread_mutexattr_t mattr;
        memset(ctx->outcome_table, 0, sizeof(OutcomeTable));

        if (pthread_mutexattr_init(&mattr) == 0) {
            pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
            pthread_mutex_init(&ctx->outcome_table->outcome_lock, &mattr);
            pthread_mutexattr_destroy(&mattr);
        }
        msync(ctx->outcome_table, sizeof(OutcomeTable), MS_SYNC);
    }

    return 0;
}

// closes/unmaps outcomes table
void ipc_close_outcome_table(ServerContext *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->outcome_table) {
        msync(ctx->outcome_table, sizeof(OutcomeTable), MS_SYNC);
        munmap(ctx->outcome_table, sizeof(OutcomeTable));
        ctx->outcome_table = NULL;
    }
    if (ctx->outcomes_fd >= 0) {
        close(ctx->outcomes_fd);
        ctx->outcomes_fd = -1;
    }
}

// full IPC init (shm/msgq/sem/files)
int ipc_initialize(ServerContext *ctx, int create_new) {
    int created = 0;

    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->shm_id = -1;
    ctx->msgq_id = -1;
    ctx->server_fd = -1;
    ctx->outcomes_fd = -1;
    ctx->running = 1;

    if (ipc_ensure_data_files() != 0) {
        perror("ensure data files");
        return -1;
    }

    if (create_new) {
        ctx->shm_id = shmget(SHM_KEY, sizeof(PlacementState), IPC_CREAT | IPC_EXCL | 0666);
        if (ctx->shm_id < 0) {
            if (errno == EEXIST) {
                ctx->shm_id = shmget(SHM_KEY, sizeof(PlacementState), 0666);
                if (ctx->shm_id < 0) {
                    perror("shmget existing");
                    return -1;
                }
            } else {
                perror("shmget create");
                return -1;
            }
        } else {
            created = 1;
        }
    } else {
        ctx->shm_id = shmget(SHM_KEY, sizeof(PlacementState), 0666);
        if (ctx->shm_id < 0) {
            perror("shmget");
            return -1;
        }
    }

    ctx->state = (PlacementState *)shmat(ctx->shm_id, NULL, 0);
    if (ctx->state == (void *)-1) {
        perror("shmat");
        ctx->state = NULL;
        return -1;
    }

    ctx->msgq_id = msgget(MSGQ_KEY, IPC_CREAT | 0666);
    if (ctx->msgq_id < 0) {
        perror("msgget");
        ipc_cleanup(ctx, 0);
        return -1;
    }

    ctx->rate_sem = sem_open(RATE_SEM_NAME, O_CREAT, 0666, MAX_CONCURRENT_CONNECTIONS);
    if (ctx->rate_sem == SEM_FAILED) {
        perror("sem_open");
        ctx->rate_sem = NULL;
        ipc_cleanup(ctx, 0);
        return -1;
    }

    if (ipc_init_outcome_table(ctx) != 0) {
        ipc_cleanup(ctx, 0);
        return -1;
    }

    if (created || ctx->state->companies[0].company_id == 0) {
        ipc_seed_default_data(ctx->state);
    }

    if (ipc_load_state(ctx) != 0) {
        ipc_save_state(ctx);
    }

    return 0;
}

// attaches shm state read-only
PlacementState *ipc_attach_state_ro(int *out_shmid) {
    int shmid = shmget(SHM_KEY, sizeof(PlacementState), 0666);
    if (shmid < 0) {
        return NULL;
    }

    PlacementState *state = (PlacementState *)shmat(shmid, NULL, SHM_RDONLY);
    if (state == (void *)-1) {
        return NULL;
    }

    if (out_shmid) {
        *out_shmid = shmid;
    }

    return state;
}

// detaches shm state ptr
int ipc_detach_state(void *state_ptr) {
    if (!state_ptr) {
        return 0;
    }
    return shmdt(state_ptr);
}

// cleanup IPC resources
void ipc_cleanup(ServerContext *ctx, int unlink_all) {
    if (!ctx) {
        return;
    }

    if (ctx->server_fd >= 0) {
        close(ctx->server_fd);
        ctx->server_fd = -1;
    }

    ipc_close_outcome_table(ctx);

    if (ctx->rate_sem) {
        sem_close(ctx->rate_sem);
        ctx->rate_sem = NULL;
    }

    if (ctx->state) {
        shmdt(ctx->state);
        ctx->state = NULL;
    }

    if (unlink_all) {
        if (ctx->shm_id >= 0) {
            shmctl(ctx->shm_id, IPC_RMID, NULL);
        }
        if (ctx->msgq_id >= 0) {
            msgctl(ctx->msgq_id, IPC_RMID, NULL);
        }
        sem_unlink(RATE_SEM_NAME);
    }

    ctx->shm_id = -1;
    ctx->msgq_id = -1;
}

// reads rate sem counts
int ipc_get_rate_sem_active(sem_t *sem, int *active, int *capacity) {
    int value = 0;
    if (!sem || !active || !capacity) {
        errno = EINVAL;
        return -1;
    }

    if (sem_getvalue(sem, &value) != 0) {
        return -1;
    }

    *capacity = MAX_CONCURRENT_CONNECTIONS;
    *active = MAX_CONCURRENT_CONNECTIONS - value;
    if (*active < 0) {
        *active = 0;
    }

    return 0;
}

// sends a packet over socket
int send_packet(int fd, const NetworkPacket *packet) {
    if (!packet) {
        errno = EINVAL;
        return -1;
    }
    return write_full(fd, packet, sizeof(*packet));
}

// receives a packet over socket
int recv_packet(int fd, NetworkPacket *packet) {
    if (!packet) {
        errno = EINVAL;
        return -1;
    }
    return read_full(fd, packet, sizeof(*packet));
}

// role enum to string
const char *role_to_string(UserRole role) {
    switch (role) {
        case ROLE_ADMIN:
            return "ADMIN";
        case ROLE_HR:
            return "HR";
        case ROLE_STUDENT:
            return "STUDENT";
        default:
            return "UNKNOWN";
    }
}

// string to role enum
UserRole role_from_string(const char *text) {
    if (!text) {
        return ROLE_STUDENT;
    }
    if (strcmp(text, "ADMIN") == 0) {
        return ROLE_ADMIN;
    }
    if (strcmp(text, "HR") == 0) {
        return ROLE_HR;
    }
    return ROLE_STUDENT;
}

// outcome enum to string
const char *outcome_to_string(InterviewOutcome outcome) {
    switch (outcome) {
        case OUTCOME_PENDING:
            return "PENDING";
        case OUTCOME_PASS:
            return "PASS";
        case OUTCOME_FAIL:
            return "FAIL";
        case OUTCOME_HOLD:
            return "HOLD";
        case OUTCOME_NO_SHOW:
            return "NO_SHOW";
        default:
            return "UNKNOWN";
    }
}

// string to outcome enum
InterviewOutcome outcome_from_string(const char *text) {
    if (!text) {
        return OUTCOME_PENDING;
    }
    if (strcmp(text, "PASS") == 0) {
        return OUTCOME_PASS;
    }
    if (strcmp(text, "FAIL") == 0) {
        return OUTCOME_FAIL;
    }
    if (strcmp(text, "HOLD") == 0) {
        return OUTCOME_HOLD;
    }
    if (strcmp(text, "NO_SHOW") == 0) {
        return OUTCOME_NO_SHOW;
    }
    return OUTCOME_PENDING;
}
