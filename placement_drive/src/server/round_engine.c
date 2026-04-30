#include "../common/models.h"

static pthread_rwlock_t g_student_locks[MAX_STUDENTS];
static int g_round_engine_initialized = 0;

int round_engine_init(void) {
    int i;
    if (g_round_engine_initialized) {
        return 0;
    }

    for (i = 0; i < MAX_STUDENTS; ++i) {
        if (pthread_rwlock_init(&g_student_locks[i], NULL) != 0) {
            int j;
            for (j = 0; j < i; ++j) {
                pthread_rwlock_destroy(&g_student_locks[j]);
            }
            return -1;
        }
    }

    g_round_engine_initialized = 1;
    return 0;
}

void round_engine_shutdown(void) {
    int i;
    if (!g_round_engine_initialized) {
        return;
    }

    for (i = 0; i < MAX_STUDENTS; ++i) {
        pthread_rwlock_destroy(&g_student_locks[i]);
    }
    g_round_engine_initialized = 0;
}

int round_engine_can_book(ServerContext *ctx, int student_id, int company_id, int round_id) {
    InterviewOutcome prev_outcome;

    if (!ctx || student_id <= 0 || student_id > MAX_STUDENTS || company_id <= 0 || company_id > MAX_COMPANIES) {
        errno = EINVAL;
        return 0;
    }

    if (round_id <= 0) {
        return 1;
    }

    if (round_id >= MAX_ROUNDS) {
        errno = EINVAL;
        return 0;
    }

    pthread_rwlock_rdlock(&g_student_locks[student_id - 1]);
    prev_outcome = outcome_tracker_get(ctx, student_id, company_id, round_id - 1);
    pthread_rwlock_unlock(&g_student_locks[student_id - 1]);

    if (prev_outcome != OUTCOME_PASS) {
        errno = EPERM;
        return 0;
    }

    return 1;
}

int round_engine_mark_outcome(ServerContext *ctx, int student_id, int company_id, int round_id, InterviewOutcome outcome) {
    char audit_detail[256];
    const char *event_name = "ROUND_HOLD";

    if (!ctx || student_id <= 0 || student_id > MAX_STUDENTS || company_id <= 0 || company_id > MAX_COMPANIES ||
        round_id < 0 || round_id >= MAX_ROUNDS) {
        errno = EINVAL;
        return -1;
    }

    pthread_rwlock_wrlock(&g_student_locks[student_id - 1]);
    int rc = outcome_tracker_set(ctx, student_id, company_id, round_id, outcome);
    pthread_rwlock_unlock(&g_student_locks[student_id - 1]);

    if (rc != 0) {
        return -1;
    }

    if (outcome == OUTCOME_PASS) {
        event_name = "ROUND_PASS";
    } else if (outcome == OUTCOME_FAIL) {
        event_name = "ROUND_FAIL";
    }

    snprintf(audit_detail, sizeof(audit_detail),
             "student=%d company=%d round=%d outcome=%s",
             student_id, company_id, round_id + 1, outcome_to_string(outcome));
    auth_append_audit(event_name, audit_detail);

    return 0;
}
