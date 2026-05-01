#include "../common/models.h"

static volatile sig_atomic_t g_running = 1;

// ctrl+c handler to stop loop
static void handle_sigint(int signo) {
    (void)signo;
    g_running = 0;
}

// draws a tiny progress bar
static void render_fill_bar(int filled, int total) {
    int width = 20;
    int lit = (total > 0) ? (filled * width) / total : 0;
    if (lit < 0) {
        lit = 0;
    }
    if (lit > width) {
        lit = width;
    }

    const char *color = "\033[31m";
    if (total > 0 && filled * 100 / total >= 70) {
        color = "\033[32m";
    } else if (total > 0 && filled * 100 / total >= 40) {
        color = "\033[33m";
    }

    printf("%s[", color);
    for (int i = 0; i < width; ++i) {
        if (i < lit) {
            putchar('#');
        } else {
            putchar('-');
        }
    }
    printf("]\033[0m");
}

// prints top few waitlist entries
static void print_top_waitlisted(const PlacementState *state, int company_id, int round_id) {
    int printed = 0;

    printf("    Waitlist top: ");
    for (int i = 0; i < state->waitlist_count && printed < 3; ++i) {
        const WaitlistEntry *entry = &state->waitlist_entries[i];
        if (entry->company_id == company_id && entry->round_id == round_id) {
            printf("[S%d %.2f slot=%d] ", entry->student_id, entry->cgpa, entry->slot_id);
            printed++;
        }
    }

    if (printed == 0) {
        printf("none");
    }
    printf("\n");
}

// renders the live board screen
static void render_board(const PlacementState *state, const OutcomeTable *table, sem_t *rate_sem) {
    time_t now = time(NULL);
    struct tm tm_now;
    int sem_value = 0;
    int active_connections = 0;

    localtime_r(&now, &tm_now);
    sem_getvalue(rate_sem, &sem_value);
    active_connections = MAX_CONCURRENT_CONNECTIONS - sem_value;
    if (active_connections < 0) {
        active_connections = 0;
    }

    printf("\033[2J\033[H");
    printf("=== CAMPUS PLACEMENT LIVE BOARD ===\n");
    printf("Updated: %04d-%02d-%02d %02d:%02d:%02d\n",
           tm_now.tm_year + 1900,
           tm_now.tm_mon + 1,
           tm_now.tm_mday,
           tm_now.tm_hour,
           tm_now.tm_min,
           tm_now.tm_sec);
    printf("Active connections: %d / %d\n\n", active_connections, MAX_CONCURRENT_CONNECTIONS);

    for (int c = 0; c < MAX_COMPANIES; ++c) {
        const Company *company = &state->companies[c];
        if (company->company_id == 0) {
            continue;
        }

        printf("%s (ID=%d)\n", company->company_name, company->company_id);

        for (int r = 0; r < company->total_rounds; ++r) {
            const InterviewRound *round = &company->rounds[r];
            int filled = round->total_slots - round->available_slots;
            int pass_count = 0;
            int fail_count = 0;

            for (int s = 0; s < MAX_STUDENTS; ++s) {
                InterviewOutcome o = table->results[s][company->company_id - 1][r];
                if (o == OUTCOME_PASS) {
                    pass_count++;
                } else if (o == OUTCOME_FAIL || o == OUTCOME_NO_SHOW) {
                    fail_count++;
                }
            }

            printf("  R%d %-12s | %2d/%2d ", r + 1, round->round_name, filled, round->total_slots);
            render_fill_bar(filled, round->total_slots);
            printf("  pass=%d fail=%d\n", pass_count, fail_count);
            print_top_waitlisted(state, company->company_id, r);
        }

        printf("\n");
    }

    printf("Press Ctrl+C to exit live board.\n");
    fflush(stdout);
}

// live board entry
int main(void) {
    int shm_id = -1;
    PlacementState *state;
    int outcomes_fd;
    OutcomeTable *table;
    sem_t *rate_sem;

    signal(SIGINT, handle_sigint);

    state = ipc_attach_state_ro(&shm_id);
    if (!state) {
        perror("shmat ro");
        return 1;
    }

    outcomes_fd = open(OUTCOMES_FILE, O_RDONLY);
    if (outcomes_fd < 0) {
        perror("open outcomes");
        ipc_detach_state(state);
        return 1;
    }

    table = mmap(NULL, sizeof(OutcomeTable), PROT_READ, MAP_SHARED, outcomes_fd, 0);
    if (table == MAP_FAILED) {
        perror("mmap outcomes");
        close(outcomes_fd);
        ipc_detach_state(state);
        return 1;
    }

    rate_sem = sem_open(RATE_SEM_NAME, 0);
    if (rate_sem == SEM_FAILED) {
        perror("sem_open");
        munmap(table, sizeof(OutcomeTable));
        close(outcomes_fd);
        ipc_detach_state(state);
        return 1;
    }

    while (g_running) {
        render_board(state, table, rate_sem);
        sleep(2);
    }

    sem_close(rate_sem);
    munmap(table, sizeof(OutcomeTable));
    close(outcomes_fd);
    ipc_detach_state(state);
    return 0;
}
