#include "../common/models.h"

// bounds check for outcome table indices
static int valid_indices(int student_id, int company_id, int round_id) {
    if (student_id <= 0 || student_id > MAX_STUDENTS) {
        return 0;
    }
    if (company_id <= 0 || company_id > MAX_COMPANIES) {
        return 0;
    }
    if (round_id < 0 || round_id >= MAX_ROUNDS) {
        return 0;
    }
    return 1;
}

// writes outcome into shared table
int outcome_tracker_set(ServerContext *ctx, int student_id, int company_id, int round_id, InterviewOutcome outcome) {
    if (!ctx || !ctx->outcome_table || !valid_indices(student_id, company_id, round_id)) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ctx->outcome_table->outcome_lock);
    ctx->outcome_table->results[student_id - 1][company_id - 1][round_id] = outcome;
    pthread_mutex_unlock(&ctx->outcome_table->outcome_lock);

    return msync(ctx->outcome_table, sizeof(OutcomeTable), MS_ASYNC);
}

// reads outcome from shared table
InterviewOutcome outcome_tracker_get(ServerContext *ctx, int student_id, int company_id, int round_id) {
    InterviewOutcome result = OUTCOME_PENDING;

    if (!ctx || !ctx->outcome_table || !valid_indices(student_id, company_id, round_id)) {
        return OUTCOME_PENDING;
    }

    pthread_mutex_lock(&ctx->outcome_table->outcome_lock);
    result = ctx->outcome_table->results[student_id - 1][company_id - 1][round_id];
    pthread_mutex_unlock(&ctx->outcome_table->outcome_lock);

    return result;
}

// async msync for outcomes table
int outcome_tracker_sync_async(ServerContext *ctx) {
    if (!ctx || !ctx->outcome_table) {
        errno = EINVAL;
        return -1;
    }
    return msync(ctx->outcome_table, sizeof(OutcomeTable), MS_ASYNC);
}

// full msync for outcomes table
int outcome_tracker_sync_full(ServerContext *ctx) {
    if (!ctx || !ctx->outcome_table) {
        errno = EINVAL;
        return -1;
    }
    return msync(ctx->outcome_table, sizeof(OutcomeTable), MS_SYNC);
}
