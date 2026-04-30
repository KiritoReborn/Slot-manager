#include "../common/models.h"

static ServerContext *g_timer_ctx = NULL;

static int get_slot_ref(ServerContext *ctx,
                        int company_id,
                        int round_id,
                        int slot_id,
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

    if (out_round) {
        *out_round = round;
    }
    if (out_slot) {
        *out_slot = &round->slots[slot_id];
    }

    return 0;
}

static void on_noshow_timeout(union sigval sigval_data) {
    BookingContext *booking = (BookingContext *)sigval_data.sival_ptr;
    ServerContext *ctx = g_timer_ctx;
    InterviewRound *round = NULL;
    InterviewSlot *slot = NULL;
    int should_promote = 0;

    if (!booking || !ctx || !ctx->state) {
        free(booking);
        return;
    }

    if (get_slot_ref(ctx, booking->company_id, booking->round_id, booking->slot_id, &round, &slot) != 0) {
        timer_delete(booking->timer_id);
        free(booking);
        return;
    }

    pthread_mutex_lock(&slot->slot_lock);

    if (slot->booking_ctx != booking) {
        pthread_mutex_unlock(&slot->slot_lock);
        timer_delete(booking->timer_id);
        free(booking);
        return;
    }

    if (slot->is_booked && slot->booked_student_id == booking->student_id && !slot->checked_in) {
        if (__sync_bool_compare_and_swap(&slot->is_booked, 1, 0)) {
            int previous_student = slot->booked_student_id;
            int new_ban_count = 0;
            char audit_line[256];

            slot->booked_student_id = -1;
            slot->checked_in = 0;
            slot->outcome = OUTCOME_NO_SHOW;
            slot->booking_epoch = 0;
            slot->booking_ctx = NULL;
            if (round->available_slots < round->total_slots) {
                round->available_slots += 1;
            }
            should_promote = 1;

            pthread_mutex_unlock(&slot->slot_lock);

            round_engine_mark_outcome(ctx, previous_student, booking->company_id, booking->round_id, OUTCOME_NO_SHOW);
            auth_update_daily_bookings(previous_student, -1, NULL);
            auth_increment_no_show(previous_student, &new_ban_count);

            snprintf(audit_line, sizeof(audit_line),
                     "NO_SHOW_PENALTY student=%d company=%d round=%d slot=%d ban_count=%d",
                     previous_student, booking->company_id, booking->round_id + 1, booking->slot_id, new_ban_count);
            auth_append_audit("NO_SHOW_PENALTY", audit_line);

            if (new_ban_count >= 3) {
                snprintf(audit_line, sizeof(audit_line), "AUTO_BAN student=%d due_to_no_show_threshold", previous_student);
                auth_append_audit("AUTO_BAN", audit_line);
            }
        } else {
            slot->booking_ctx = NULL;
            pthread_mutex_unlock(&slot->slot_lock);
        }
    } else {
        slot->booking_ctx = NULL;
        pthread_mutex_unlock(&slot->slot_lock);
    }

    timer_delete(booking->timer_id);
    if (should_promote) {
        slot_manager_promote_waitlist(ctx, booking->company_id, booking->round_id, booking->slot_id);
    }
    free(booking);
}

void timer_manager_init(ServerContext *ctx) {
    g_timer_ctx = ctx;
}

void timer_manager_shutdown(ServerContext *ctx) {
    int c;
    int r;
    int s;

    if (!ctx || !ctx->state) {
        return;
    }

    for (c = 0; c < MAX_COMPANIES; ++c) {
        Company *company = &ctx->state->companies[c];
        for (r = 0; r < company->total_rounds; ++r) {
            InterviewRound *round = &company->rounds[r];
            for (s = 0; s < round->total_slots; ++s) {
                timer_manager_disarm(ctx, company->company_id, r, s);
            }
        }
    }
}

int timer_manager_arm(ServerContext *ctx, int student_id, int company_id, int round_id, int slot_id, int deadline_sec) {
    InterviewSlot *slot;
    BookingContext *booking;
    struct sigevent sev;
    struct itimerspec its;

    if (get_slot_ref(ctx, company_id, round_id, slot_id, NULL, &slot) != 0 || deadline_sec <= 0) {
        return -1;
    }

    booking = calloc(1, sizeof(*booking));
    if (!booking) {
        return -1;
    }

    booking->student_id = student_id;
    booking->company_id = company_id;
    booking->round_id = round_id;
    booking->slot_id = slot_id;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = on_noshow_timeout;
    sev.sigev_value.sival_ptr = booking;

    if (timer_create(CLOCK_REALTIME, &sev, &booking->timer_id) != 0) {
        free(booking);
        return -1;
    }

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = deadline_sec;
    its.it_interval.tv_sec = 0;

    if (timer_settime(booking->timer_id, 0, &its, NULL) != 0) {
        timer_delete(booking->timer_id);
        free(booking);
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (slot->booking_ctx) {
        timer_delete(slot->booking_ctx->timer_id);
        free(slot->booking_ctx);
    }
    slot->booking_ctx = booking;
    pthread_mutex_unlock(&slot->slot_lock);

    return 0;
}

int timer_manager_disarm(ServerContext *ctx, int company_id, int round_id, int slot_id) {
    InterviewSlot *slot;

    if (get_slot_ref(ctx, company_id, round_id, slot_id, NULL, &slot) != 0) {
        return -1;
    }

    pthread_mutex_lock(&slot->slot_lock);
    if (slot->booking_ctx) {
        timer_delete(slot->booking_ctx->timer_id);
        free(slot->booking_ctx);
        slot->booking_ctx = NULL;
    }
    pthread_mutex_unlock(&slot->slot_lock);

    return 0;
}
