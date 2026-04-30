#include "../common/models.h"

static int get_slot_ref(ServerContext *ctx,
                        int company_id,
                        int round_id,
                        int slot_id,
                        Company **out_company,
                        InterviewRound **out_round,
                        InterviewSlot **out_slot) {
    Company *company;
    InterviewRound *round;

    if (!ctx || !ctx->state || company_id <= 0 || company_id > MAX_COMPANIES) {
        errno = EINVAL;
        return -1;
    }

    company = &ctx->state->companies[company_id - 1];
    if (round_id < 0 || round_id >= company->total_rounds) {
        errno = EINVAL;
        return -1;
    }

    round = &company->rounds[round_id];
    if (slot_id < 0 || slot_id >= round->total_slots) {
        errno = EINVAL;
        return -1;
    }

    if (out_company) {
        *out_company = company;
    }
    if (out_round) {
        *out_round = round;
    }
    if (out_slot) {
        *out_slot = &round->slots[slot_id];
    }

    return 0;
}

static int student_has_booking_in_company(ServerContext *ctx, int student_id, int company_id) {
    Company *company;

    if (!ctx || !ctx->state || student_id <= 0 || company_id <= 0 || company_id > MAX_COMPANIES) {
        return 0;
    }

    company = &ctx->state->companies[company_id - 1];
    for (int r = 0; r < company->total_rounds; ++r) {
        InterviewRound *round = &company->rounds[r];
        for (int s = 0; s < round->total_slots; ++s) {
            InterviewSlot *slot = &round->slots[s];
            pthread_mutex_lock(&slot->slot_lock);
            int matched = (slot->is_booked && slot->booked_student_id == student_id);
            pthread_mutex_unlock(&slot->slot_lock);
            if (matched) {
                return 1;
            }
        }
    }

    return 0;
}

static long waitlist_priority_from_cgpa(float cgpa) {
    long mtype = (long)(1000 - (cgpa * 100.0f) + 0.5f);
    if (mtype < 1) {
        mtype = 1;
    }
    if (mtype > WAITLIST_MAX_MTYPE) {
        mtype = WAITLIST_MAX_MTYPE;
    }
    return mtype;
}

static void waitlist_shared_insert(PlacementState *state, const struct waitlist_msgbuf *msg) {
    int i;

    pthread_mutex_lock(&state->waitlist_lock);

    if (state->waitlist_count < MAX_WAITLIST_ENTRIES) {
        WaitlistEntry entry;
        entry.priority = msg->mtype;
        entry.student_id = msg->student_id;
        entry.company_id = msg->company_id;
        entry.round_id = msg->round_id;
        entry.slot_id = msg->requested_slot_id;
        entry.cgpa = msg->cgpa;
        strncpy(entry.student_name, msg->student_name, sizeof(entry.student_name) - 1);
        entry.student_name[sizeof(entry.student_name) - 1] = '\0';

        state->waitlist_entries[state->waitlist_count++] = entry;

        for (i = state->waitlist_count - 1;
             i > 0 && state->waitlist_entries[i].priority < state->waitlist_entries[i - 1].priority;
             --i) {
            WaitlistEntry temp = state->waitlist_entries[i];
            state->waitlist_entries[i] = state->waitlist_entries[i - 1];
            state->waitlist_entries[i - 1] = temp;
        }
    }

    pthread_mutex_unlock(&state->waitlist_lock);
}

static void waitlist_shared_remove(PlacementState *state, int student_id, int company_id, int round_id, int slot_id) {
    int i;

    pthread_mutex_lock(&state->waitlist_lock);

    for (i = 0; i < state->waitlist_count; ++i) {
        WaitlistEntry *entry = &state->waitlist_entries[i];
        if (entry->student_id == student_id && entry->company_id == company_id && entry->round_id == round_id &&
            entry->slot_id == slot_id) {
            int j;
            for (j = i; j < state->waitlist_count - 1; ++j) {
                state->waitlist_entries[j] = state->waitlist_entries[j + 1];
            }
            state->waitlist_count--;
            break;
        }
    }

    pthread_mutex_unlock(&state->waitlist_lock);
}

int slot_manager_enqueue_waitlist(ServerContext *ctx,
                                  int student_id,
                                  const char *student_name,
                                  float cgpa,
                                  int company_id,
                                  int round_id,
                                  int slot_id) {
    struct waitlist_msgbuf msg;
    char audit_detail[256];

    if (!ctx || !ctx->state || !student_name) {
        errno = EINVAL;
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    msg.mtype = waitlist_priority_from_cgpa(cgpa);
    msg.student_id = student_id;
    msg.requested_slot_id = slot_id;
    msg.company_id = company_id;
    msg.round_id = round_id;
    msg.cgpa = cgpa;
    strncpy(msg.student_name, student_name, sizeof(msg.student_name) - 1);

    if (msgsnd(ctx->msgq_id, &msg, sizeof(msg) - sizeof(long), IPC_NOWAIT) != 0) {
        return -1;
    }

    waitlist_shared_insert(ctx->state, &msg);

    snprintf(audit_detail, sizeof(audit_detail),
             "WAITLIST_ENQUEUE student=%d company=%d round=%d slot=%d cgpa=%.2f priority=%ld",
             student_id, company_id, round_id + 1, slot_id, cgpa, msg.mtype);
    auth_append_audit("WAITLIST_ENQUEUE", audit_detail);

    ipc_save_state(ctx);

    return 0;
}

static int dequeue_waitlist_for_slot(ServerContext *ctx, int company_id, int round_id, int slot_id, struct waitlist_msgbuf *out_msg) {
    struct waitlist_msgbuf deferred[64];
    int deferred_count = 0;
    int found = 0;

    while (1) {
        struct waitlist_msgbuf msg;
        ssize_t rc = msgrcv(ctx->msgq_id, &msg, sizeof(msg) - sizeof(long), -WAITLIST_MAX_MTYPE, IPC_NOWAIT);
        if (rc < 0) {
            if (errno == ENOMSG) {
                break;
            }
            return -1;
        }

        if (msg.company_id == company_id && msg.round_id == round_id && msg.requested_slot_id == slot_id) {
            *out_msg = msg;
            found = 1;
            break;
        }

        if (deferred_count < (int)(sizeof(deferred) / sizeof(deferred[0]))) {
            deferred[deferred_count++] = msg;
        }
    }

    for (int i = 0; i < deferred_count; ++i) {
        msgsnd(ctx->msgq_id, &deferred[i], sizeof(deferred[i]) - sizeof(long), IPC_NOWAIT);
    }

    if (!found) {
        errno = ENOMSG;
        return -1;
    }

    return 0;
}

int slot_manager_promote_waitlist(ServerContext *ctx, int company_id, int round_id, int slot_id) {
    Company *company;
    InterviewRound *round;
    InterviewSlot *slot;
    struct waitlist_msgbuf promoted;
    char audit_detail[256];
    NotifPacket notif;

    if (get_slot_ref(ctx, company_id, round_id, slot_id, &company, &round, &slot) != 0) {
        return -1;
    }

    if (dequeue_waitlist_for_slot(ctx, company_id, round_id, slot_id, &promoted) != 0) {
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (slot->is_booked) {
        pthread_mutex_unlock(&slot->slot_lock);
        errno = EBUSY;
        return -1;
    }

    slot->is_booked = 1;
    slot->checked_in = 0;
    slot->booked_student_id = promoted.student_id;
    slot->outcome = OUTCOME_PENDING;
    slot->booking_epoch = time(NULL);

    if (round->available_slots > 0) {
        round->available_slots -= 1;
    }

    pthread_mutex_unlock(&slot->slot_lock);

    timer_manager_arm(ctx, promoted.student_id, company_id, round_id, slot_id, CHECKIN_DEADLINE_SECONDS);
    waitlist_shared_remove(ctx->state, promoted.student_id, company_id, round_id, slot_id);

    memset(&notif, 0, sizeof(notif));
    notif.type = NOTIF_PROMOTED;
    notif.company_id = company_id;
    notif.round_id = round_id;
    notif.slot_id = slot_id;
    snprintf(notif.message, sizeof(notif.message), "Promoted from waitlist: %s R%d %s",
             company->company_name, round_id + 1, slot->time_window);
    fifo_send_notification(promoted.student_id, &notif);

    snprintf(audit_detail, sizeof(audit_detail),
             "student=%d promoted company=%d round=%d slot=%d",
             promoted.student_id, company_id, round_id + 1, slot_id);
    auth_append_audit("WAITLIST_PROMOTED", audit_detail);

    ipc_save_state(ctx);

    return 0;
}

int slot_manager_book(ServerContext *ctx,
                      int student_id,
                      const char *student_name,
                      float cgpa,
                      int company_id,
                      int round_id,
                      int slot_id) {
    Company *company;
    InterviewRound *round;
    InterviewSlot *slot;
    char audit_detail[256];

    if (get_slot_ref(ctx, company_id, round_id, slot_id, &company, &round, &slot) != 0) {
        return -1;
    }

    if (student_has_booking_in_company(ctx, student_id, company_id)) {
        errno = EALREADY;
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (!slot->is_booked) {
        int daily = 0;
        slot->is_booked = 1;
        slot->checked_in = 0;
        slot->booked_student_id = student_id;
        slot->outcome = OUTCOME_PENDING;
        slot->booking_epoch = time(NULL);
        if (round->available_slots > 0) {
            round->available_slots -= 1;
        }
        pthread_mutex_unlock(&slot->slot_lock);

        timer_manager_arm(ctx, student_id, company_id, round_id, slot_id, CHECKIN_DEADLINE_SECONDS);
        auth_update_daily_bookings(student_id, +1, &daily);

        snprintf(audit_detail, sizeof(audit_detail),
                 "BOOK_SUCCESS student=%d company=%d round=%d slot=%d",
                 student_id, company_id, round_id + 1, slot_id);
        auth_append_audit("BOOK_SUCCESS", audit_detail);
        ipc_save_state(ctx);
        return 0;
    }
    pthread_mutex_unlock(&slot->slot_lock);

    if (slot_manager_enqueue_waitlist(ctx, student_id, student_name, cgpa, company_id, round_id, slot_id) != 0) {
        return -1;
    }

    return 1;
}

int slot_manager_cancel(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id, int force) {
    Company *company;
    InterviewRound *round;
    InterviewSlot *slot;
    int previous_student;
    char audit_detail[256];

    if (get_slot_ref(ctx, company_id, round_id, slot_id, &company, &round, &slot) != 0) {
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (!slot->is_booked) {
        pthread_mutex_unlock(&slot->slot_lock);
        errno = ENOENT;
        return -1;
    }

    previous_student = slot->booked_student_id;
    if (!force && previous_student != student_id) {
        pthread_mutex_unlock(&slot->slot_lock);
        errno = EPERM;
        return -1;
    }

    slot->is_booked = 0;
    slot->checked_in = 0;
    slot->booked_student_id = -1;
    slot->outcome = OUTCOME_PENDING;
    slot->booking_epoch = 0;

    if (round->available_slots < round->total_slots) {
        round->available_slots += 1;
    }

    pthread_mutex_unlock(&slot->slot_lock);

    timer_manager_disarm(ctx, company_id, round_id, slot_id);
    if (previous_student > 0) {
        int daily = 0;
        auth_update_daily_bookings(previous_student, -1, &daily);
    }

    snprintf(audit_detail, sizeof(audit_detail),
             "CANCEL slot company=%d round=%d slot=%d by_user=%d force=%d",
             company_id, round_id + 1, slot_id, student_id, force);
    auth_append_audit("CANCEL", audit_detail);

    slot_manager_promote_waitlist(ctx, company_id, round_id, slot_id);
    ipc_save_state(ctx);
    return 0;
}

int slot_manager_checkin(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id) {
    Company *company;
    InterviewRound *round;
    InterviewSlot *slot;
    char audit_detail[256];

    (void)company;
    (void)round;

    if (get_slot_ref(ctx, company_id, round_id, slot_id, &company, &round, &slot) != 0) {
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (!slot->is_booked || slot->booked_student_id != student_id) {
        pthread_mutex_unlock(&slot->slot_lock);
        errno = EPERM;
        return -1;
    }

    slot->checked_in = 1;
    pthread_mutex_unlock(&slot->slot_lock);

    timer_manager_disarm(ctx, company_id, round_id, slot_id);

    snprintf(audit_detail, sizeof(audit_detail),
             "CHECKIN student=%d company=%d round=%d slot=%d",
             student_id, company_id, round_id + 1, slot_id);
    auth_append_audit("CHECKIN", audit_detail);

    ipc_save_state(ctx);

    return 0;
}

int slot_manager_atomic_swap(ServerContext *ctx,
                             int student_a,
                             int company_a,
                             int round_a,
                             int slot_a,
                             int student_b,
                             int company_b,
                             int round_b,
                             int slot_b) {
    InterviewSlot *s1;
    InterviewSlot *s2;
    char audit_detail[256];
    int attempt;

    if (get_slot_ref(ctx, company_a, round_a, slot_a, NULL, NULL, &s1) != 0) {
        return -1;
    }
    if (get_slot_ref(ctx, company_b, round_b, slot_b, NULL, NULL, &s2) != 0) {
        return -1;
    }

    for (attempt = 0; attempt < 16; ++attempt) {
        if (s1->booked_student_id != student_a || s2->booked_student_id != student_b) {
            errno = EPERM;
            return -1;
        }

        if (!__sync_bool_compare_and_swap(&s1->booked_student_id, student_a, -1)) {
            continue;
        }

        if (!__sync_bool_compare_and_swap(&s2->booked_student_id, student_b, student_a)) {
            __sync_bool_compare_and_swap(&s1->booked_student_id, -1, student_a);
            continue;
        }

        if (__sync_bool_compare_and_swap(&s1->booked_student_id, -1, student_b)) {
            snprintf(audit_detail, sizeof(audit_detail),
                     "SWAP_COMMIT A(student=%d c=%d r=%d s=%d) <-> B(student=%d c=%d r=%d s=%d)",
                     student_a, company_a, round_a + 1, slot_a,
                     student_b, company_b, round_b + 1, slot_b);
            auth_append_audit("SWAP_COMMIT", audit_detail);
            ipc_save_state(ctx);
            return 0;
        }

        __sync_bool_compare_and_swap(&s2->booked_student_id, student_a, student_b);
        __sync_bool_compare_and_swap(&s1->booked_student_id, -1, student_a);
    }

    errno = EBUSY;
    return -1;
}

int slot_manager_add_slot(ServerContext *ctx, int company_id, int round_id, const char *time_window) {
    Company *company;
    InterviewRound *round;
    InterviewSlot *slot;
    char audit_detail[256];

    if (!ctx || !ctx->state || !time_window || company_id <= 0 || company_id > MAX_COMPANIES) {
        errno = EINVAL;
        return -1;
    }

    company = &ctx->state->companies[company_id - 1];
    if (round_id < 0 || round_id >= company->total_rounds) {
        errno = EINVAL;
        return -1;
    }

    pthread_rwlock_wrlock(&company->company_lock);
    round = &company->rounds[round_id];
    if (round->total_slots >= MAX_SLOTS_PER_ROUND) {
        pthread_rwlock_unlock(&company->company_lock);
        errno = ENOSPC;
        return -1;
    }

    slot = &round->slots[round->total_slots];
    memset(slot, 0, sizeof(*slot));
    slot->slot_id = round->total_slots;
    strncpy(slot->time_window, time_window, sizeof(slot->time_window) - 1);
    slot->booked_student_id = -1;
    slot->outcome = OUTCOME_PENDING;
    slot->checked_in = 0;
    pthread_mutex_init(&slot->slot_lock, &ctx->state->shm_mutexattr);

    round->total_slots += 1;
    round->available_slots += 1;

    pthread_rwlock_unlock(&company->company_lock);

    snprintf(audit_detail, sizeof(audit_detail),
             "company=%d round=%d slot=%d time=%s",
             company_id, round_id + 1, slot->slot_id, slot->time_window);
    auth_append_audit("HR_ADD_SLOT", audit_detail);

    ipc_save_state(ctx);

    return 0;
}

int slot_manager_finalize_after_outcome(ServerContext *ctx,
                                        int student_id,
                                        int company_id,
                                        int round_id,
                                        InterviewOutcome outcome) {
    Company *company;
    InterviewRound *round;
    int finalized = 0;
    char audit_detail[256];

    if (!ctx || !ctx->state || student_id <= 0 || company_id <= 0 || company_id > MAX_COMPANIES) {
        errno = EINVAL;
        return -1;
    }

    company = &ctx->state->companies[company_id - 1];
    if (round_id < 0 || round_id >= company->total_rounds) {
        errno = EINVAL;
        return -1;
    }

    round = &company->rounds[round_id];
    for (int s = 0; s < round->total_slots; ++s) {
        InterviewSlot *slot = &round->slots[s];

        pthread_mutex_lock(&slot->slot_lock);
        if (slot->is_booked && slot->booked_student_id == student_id) {
            slot->outcome = outcome;
            slot->is_booked = 0;
            slot->checked_in = 0;
            slot->booked_student_id = -1;
            slot->booking_epoch = 0;
            if (round->available_slots < round->total_slots) {
                round->available_slots += 1;
            }
            finalized += 1;
            pthread_mutex_unlock(&slot->slot_lock);

            timer_manager_disarm(ctx, company_id, round_id, slot->slot_id);
            slot_manager_promote_waitlist(ctx, company_id, round_id, slot->slot_id);
            continue;
        }
        pthread_mutex_unlock(&slot->slot_lock);
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "OUTCOME_FINALIZE student=%d company=%d round=%d outcome=%s finalized_slots=%d",
             student_id,
             company_id,
             round_id + 1,
             outcome_to_string(outcome),
             finalized);
    auth_append_audit("OUTCOME_FINALIZE", audit_detail);

    ipc_save_state(ctx);

    return finalized;
}
