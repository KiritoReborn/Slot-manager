#include "../common/models.h"

#include <arpa/inet.h>
#include <sys/wait.h>

typedef struct {
    ServerContext *ctx;
    int client_fd;
    struct sockaddr_in addr;
} HandlerArgs;

static ServerContext g_ctx;
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_overrun_broadcast_requested = 0;

static void decrease_active_connection_count(ServerContext *ctx) {
    if (!ctx || !ctx->state) {
        return;
    }

    pthread_mutex_lock(&ctx->state->shm_lock);
    if (ctx->state->active_student_connections > 0) {
        ctx->state->active_student_connections -= 1;
    }
    pthread_mutex_unlock(&ctx->state->shm_lock);

    if (ctx->rate_sem) {
        sem_post(ctx->rate_sem);
    }
}

static void increase_active_connection_count(ServerContext *ctx) {
    if (!ctx || !ctx->state) {
        return;
    }

    pthread_mutex_lock(&ctx->state->shm_lock);
    ctx->state->active_student_connections += 1;
    pthread_mutex_unlock(&ctx->state->shm_lock);
}

static void signal_sigint(int signo) {
    (void)signo;
    g_stop_requested = 1;
    if (g_ctx.server_fd >= 0) {
        close(g_ctx.server_fd);
        g_ctx.server_fd = -1;
    }
}

static void signal_sigusr2(int signo) {
    (void)signo;
    g_overrun_broadcast_requested = 1;
}

static void signal_sigchld(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static int install_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_sigusr2;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, NULL) != 0) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        return -1;
    }

    return 0;
}

static void send_reply(int fd, MessageType type, int user_id, const char *payload) {
    NetworkPacket response;
    memset(&response, 0, sizeof(response));
    response.type = type;
    response.user_id = user_id;
    if (payload) {
        strncpy(response.payload, payload, sizeof(response.payload) - 1);
    }
    send_packet(fd, &response);
}

static void render_view_summary(ServerContext *ctx, char *out, size_t out_sz) {
    int active = 0;
    int capacity = 0;
    size_t used = 0;

    if (!ctx || !ctx->state || !out || out_sz == 0) {
        return;
    }

    ipc_get_rate_sem_active(ctx->rate_sem, &active, &capacity);
    used += (size_t)snprintf(out + used, out_sz - used, "Active connections: %d/%d | ", active, capacity);

    for (int i = 0; i < 3 && i < MAX_COMPANIES; ++i) {
        Company *company = &ctx->state->companies[i];
        if (company->company_id == 0) {
            continue;
        }
        for (int r = 0; r < company->total_rounds; ++r) {
            InterviewRound *round = &company->rounds[r];
            used += (size_t)snprintf(out + used, out_sz - used,
                                     "%s-R%d:%d/%d ",
                                     company->company_name,
                                     r + 1,
                                     round->total_slots - round->available_slots,
                                     round->total_slots);
            if (used + 32 >= out_sz) {
                return;
            }
        }
    }
}

static int enforce_student_booking_limit(int student_id) {
    UserRecord user;
    if (auth_lookup_user_by_id(student_id, &user) != 0) {
        return -1;
    }
    if (user.daily_bookings >= 3) {
        errno = EUSERS;
        return -1;
    }
    return 0;
}

static int parse_swap_target(const char *payload, int *company_id, int *round_id, int *slot_id) {
    if (!payload || !company_id || !round_id || !slot_id) {
        return -1;
    }
    return sscanf(payload, "%d %d %d", company_id, round_id, slot_id) == 3 ? 0 : -1;
}

static int parse_time_window_minutes(const char *window, int *start_min, int *end_min) {
    int sh;
    int sm;
    int eh;
    int em;

    if (!window || !start_min || !end_min) {
        return -1;
    }

    if (sscanf(window, "%d:%d-%d:%d", &sh, &sm, &eh, &em) != 4) {
        return -1;
    }
    if (sh < 0 || sh > 23 || eh < 0 || eh > 23 || sm < 0 || sm > 59 || em < 0 || em > 59) {
        return -1;
    }

    *start_min = sh * 60 + sm;
    *end_min = eh * 60 + em;
    return 0;
}

static void format_time_window_minutes(int start_min, int end_min, char *out, size_t out_sz) {
    int sh = (start_min / 60) % 24;
    int sm = start_min % 60;
    int eh = (end_min / 60) % 24;
    int em = end_min % 60;
    snprintf(out, out_sz, "%02d:%02d-%02d:%02d", sh, sm, eh, em);
}

static int find_first_free_slot(ServerContext *ctx, int company_id, int round_id, int *out_slot_id) {
    if (!ctx || !ctx->state || !out_slot_id || company_id <= 0 || company_id > MAX_COMPANIES) {
        return -1;
    }

    Company *company = &ctx->state->companies[company_id - 1];
    if (round_id < 0 || round_id >= company->total_rounds) {
        return -1;
    }

    InterviewRound *round = &company->rounds[round_id];
    for (int s = 0; s < round->total_slots; ++s) {
        InterviewSlot *slot = &round->slots[s];
        pthread_mutex_lock(&slot->slot_lock);
        int free_slot = !slot->is_booked;
        pthread_mutex_unlock(&slot->slot_lock);
        if (free_slot) {
            *out_slot_id = s;
            return 0;
        }
    }

    return -1;
}

static int shift_overrun_and_queue_alerts(ServerContext *ctx,
                                          int company_id,
                                          int round_id,
                                          int slot_id,
                                          int delay_minutes) {
    char audit_detail[256];

    if (!ctx || !ctx->state || company_id <= 0 || company_id > MAX_COMPANIES || delay_minutes <= 0) {
        errno = EINVAL;
        return -1;
    }

    Company *company = &ctx->state->companies[company_id - 1];
    if (round_id < 0 || round_id >= company->total_rounds) {
        errno = EINVAL;
        return -1;
    }

    InterviewRound *round = &company->rounds[round_id];
    if (slot_id < 0 || slot_id >= round->total_slots) {
        errno = EINVAL;
        return -1;
    }

    int affected_students = 0;
    for (int s = slot_id + 1; s < round->total_slots; ++s) {
        InterviewSlot *slot = &round->slots[s];
        int start_min;
        int end_min;
        int booked_student;

        pthread_mutex_lock(&slot->slot_lock);
        if (parse_time_window_minutes(slot->time_window, &start_min, &end_min) == 0) {
            start_min += delay_minutes;
            end_min += delay_minutes;
            format_time_window_minutes(start_min, end_min, slot->time_window, sizeof(slot->time_window));
        }
        booked_student = slot->is_booked ? slot->booked_student_id : -1;

        if (booked_student > 0) {
            struct overrun_msgbuf msg;
            memset(&msg, 0, sizeof(msg));
            msg.mtype = OVERRUN_ALERT_MTYPE;
            msg.student_id = booked_student;
            msg.company_id = company_id;
            msg.slot_id = slot->slot_id;
            msg.delay_minutes = delay_minutes;
            strncpy(msg.new_time_window, slot->time_window, sizeof(msg.new_time_window) - 1);
            msgsnd(ctx->msgq_id, &msg, sizeof(msg) - sizeof(long), IPC_NOWAIT);
            affected_students += 1;
        }
        pthread_mutex_unlock(&slot->slot_lock);
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "company=%d round=%d anchor_slot=%d delay_min=%d affected_students=%d",
             company_id,
             round_id + 1,
             slot_id,
             delay_minutes,
             affected_students);
    auth_append_audit("INTERVIEW_OVERRUN", audit_detail);

    return affected_students;
}

static void process_overrun_alerts(ServerContext *ctx) {
    while (1) {
        struct overrun_msgbuf msg;
        NotifPacket notif;
        char audit_detail[256];
        ssize_t rc = msgrcv(ctx->msgq_id,
                            &msg,
                            sizeof(msg) - sizeof(long),
                            OVERRUN_ALERT_MTYPE,
                            IPC_NOWAIT);

        if (rc < 0) {
            if (errno == ENOMSG) {
                break;
            }
            return;
        }

        memset(&notif, 0, sizeof(notif));
        notif.type = NOTIF_OVERRUN;
        notif.company_id = msg.company_id;
        notif.round_id = 0;
        notif.slot_id = msg.slot_id;
        snprintf(notif.message,
                 sizeof(notif.message),
                 "Interview delayed by %d min. New slot time: %s",
                 msg.delay_minutes,
                 msg.new_time_window);
        fifo_send_notification(msg.student_id, &notif);

        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "student=%d company=%d slot=%d delay=%d new_time=%s",
                 msg.student_id,
                 msg.company_id,
                 msg.slot_id,
                 msg.delay_minutes,
                 msg.new_time_window);
        auth_append_audit("OVERRUN_ALERT", audit_detail);
    }
}

static void handle_student_request(ServerContext *ctx, int fd, const UserRecord *user, const NetworkPacket *req) {
    int banned = 0;

    if (auth_is_banned(user->id, &banned) == 0 && banned) {
        send_reply(fd, REP_FAIL, user->id, "Account banned due to policy violations.");
        return;
    }

    switch (req->type) {
        case REQ_VIEW: {
            char summary[256] = {0};
            render_view_summary(ctx, summary, sizeof(summary));
            send_reply(fd, REP_SUCCESS, user->id, summary);
            break;
        }
        case REQ_BOOK: {
            float cgpa = 0.0f;
            char student_name[64] = {0};
            if (enforce_student_booking_limit(user->id) != 0) {
                send_reply(fd, REP_FAIL, user->id, "Daily booking limit reached (max 3).");
                break;
            }
            if (auth_lookup_cgpa(user->id, &cgpa, student_name, sizeof(student_name)) != 0) {
                send_reply(fd, REP_FAIL, user->id, "Unable to load profile CGPA.");
                break;
            }

            int rc = slot_manager_book(ctx, user->id, student_name, cgpa, req->company_id, req->round_id, req->slot_id);
            if (rc == 0) {
                ipc_save_state(ctx);
                send_reply(fd, REP_SUCCESS, user->id, "Slot booked and check-in timer started.");
            } else if (rc == 1) {
                ipc_save_state(ctx);
                send_reply(fd, REP_WAITLIST_PROMOTED, user->id,
                           "Slot busy. Added to priority waitlist (CGPA-based). Waiting for promotion.");
            } else if (errno == EALREADY) {
                send_reply(fd, REP_FAIL, user->id, "You already have a booking in this company. Cancel it before booking another slot.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Booking failed. Check company id and slot id.");
            }
            break;
        }
        case REQ_CANCEL: {
            if (slot_manager_cancel(ctx, user->id, req->company_id, req->round_id, req->slot_id, 0) == 0) {
                ipc_save_state(ctx);
                send_reply(fd, REP_SUCCESS, user->id, "Booking canceled.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Cancel failed.");
            }
            break;
        }
        case REQ_CHECKIN: {
            if (slot_manager_checkin(ctx, user->id, req->company_id, req->round_id, req->slot_id) == 0) {
                ipc_save_state(ctx);
                send_reply(fd, REP_SUCCESS, user->id, "Checked in successfully.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Check-in failed.");
            }
            break;
        }
        case REQ_JOIN_WAITING_ROOM: {
            if (waiting_room_join(req->company_id, user->id) == 0) {
                char line[128];
                snprintf(line, sizeof(line), "student=%d company=%d", user->id, req->company_id);
                auth_append_audit("WAITING_ROOM_JOIN", line);
                send_reply(fd, REP_SUCCESS, user->id, "Joined waiting room. HR will call next candidate.");
            } else if (errno == EALREADY) {
                send_reply(fd, REP_FAIL, user->id, "You are already in this waiting room queue.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Failed to join waiting room.");
            }
            break;
        }
        case REQ_SWAP_PROPOSE:
        case REQ_SWAP_ACCEPT: {
            int target_company = 0;
            int target_round = 0;
            int target_slot = 0;
            char audit_line[256];
            NotifPacket notif;

            if (parse_swap_target(req->payload, &target_company, &target_round, &target_slot) != 0) {
                send_reply(fd, REP_FAIL, user->id, "Swap payload must be: <company> <round_index> <slot_id>.");
                break;
            }

            memset(&notif, 0, sizeof(notif));
            notif.type = NOTIF_SWAP_REQUEST;
            notif.company_id = req->company_id;
            notif.round_id = req->round_id;
            notif.slot_id = req->slot_id;
            snprintf(notif.message, sizeof(notif.message),
                     "Swap request from student %d for c=%d r=%d s=%d",
                     user->id, req->company_id, req->round_id, req->slot_id);
            fifo_send_notification(req->target_student_id, &notif);

            snprintf(audit_line, sizeof(audit_line),
                     "SWAP_REQUEST from=%d to=%d source=(%d,%d,%d) target=(%d,%d,%d)",
                     user->id,
                     req->target_student_id,
                     req->company_id,
                     req->round_id,
                     req->slot_id,
                     target_company,
                     target_round,
                     target_slot);
            auth_append_audit("SWAP_REQUEST", audit_line);

            if (req->type == REQ_SWAP_ACCEPT) {
                if (slot_manager_atomic_swap(ctx,
                                             user->id, req->company_id, req->round_id, req->slot_id,
                                             req->target_student_id, target_company, target_round, target_slot) == 0) {
                    send_reply(fd, REP_SUCCESS, user->id, "Swap committed.");
                } else {
                    send_reply(fd, REP_FAIL, user->id, "Swap failed due to race or mismatched ownership.");
                }
            } else {
                send_reply(fd, REP_ALERT, user->id, "Swap request dispatched to target student via FIFO.");
            }
            break;
        }
        default:
            send_reply(fd, REP_FAIL, user->id, "Unsupported student command.");
            break;
    }
}

static void handle_hr_request(ServerContext *ctx, int fd, const UserRecord *user, const NetworkPacket *req) {

    switch (req->type) {
        case REQ_VIEW: {
            char summary[256] = {0};
            render_view_summary(ctx, summary, sizeof(summary));
            send_reply(fd, REP_SUCCESS, user->id, summary);
            break;
        }
        case REQ_HR_ADD_SLOT: {
            if (slot_manager_add_slot(ctx, req->company_id, req->round_id, req->payload) == 0) {
                ipc_save_state(ctx);
                send_reply(fd, REP_SUCCESS, user->id, "Slot added.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Failed to add slot.");
            }
            break;
        }
        case REQ_HR_MARK_OUTCOME: {
            InterviewOutcome outcome = outcome_from_string(req->payload);
            if (round_engine_mark_outcome(ctx, req->target_student_id, req->company_id, req->round_id, outcome) == 0) {
                int released = slot_manager_finalize_after_outcome(ctx,
                                                                   req->target_student_id,
                                                                   req->company_id,
                                                                   req->round_id,
                                                                   outcome);
                ipc_save_state(ctx);
                char msg[256];
                snprintf(msg,
                         sizeof(msg),
                         "Outcome updated. Released slots for student: %d",
                         released < 0 ? 0 : released);
                send_reply(fd, REP_SUCCESS, user->id, msg);
            } else {
                send_reply(fd, REP_FAIL, user->id, "Failed to mark outcome.");
            }
            break;
        }
        case REQ_HR_NEXT_CANDIDATE: {
            int company_id = user->company_id;
            int student_id = -1;

            if (company_id <= 0 && req->company_id > 0) {
                company_id = req->company_id;
            }

            if (waiting_room_try_next_candidate(company_id, &student_id) != 0) {
                if (errno == EAGAIN) {
                    send_reply(fd, REP_FAIL, user->id, "No students are currently in the waiting room queue.");
                } else {
                    send_reply(fd, REP_FAIL, user->id, "No candidate dispatched due to queue error.");
                }
                break;
            }

            int slot_id;
            if (find_first_free_slot(ctx, company_id, 0, &slot_id) == 0) {
                float cgpa = 0.0f;
                char student_name[64] = {0};
                if (auth_lookup_cgpa(student_id, &cgpa, student_name, sizeof(student_name)) == 0) {
                    int rc = slot_manager_book(ctx, student_id, student_name, cgpa, company_id, 0, slot_id);
                    if (rc == 0 || rc == 1) {
                        NotifPacket notif;
                        char msg[256];
                        memset(&notif, 0, sizeof(notif));
                        notif.type = NOTIF_WAITING_ROOM_CALL;
                        notif.company_id = company_id;
                        notif.round_id = 0;
                        notif.slot_id = slot_id;
                        snprintf(notif.message,
                                 sizeof(notif.message),
                                 "You were called from waiting room. Slot %d assigned.",
                                 slot_id);
                        fifo_send_notification(student_id, &notif);

                        snprintf(msg,
                                 sizeof(msg),
                                 "Next candidate: %s (ID=%d). Assigned slot=%d.",
                                 student_name,
                                 student_id,
                                 slot_id);
                        send_reply(fd, REP_SUCCESS, user->id, msg);
                        ipc_save_state(ctx);
                        break;
                    }
                }
            }

            waiting_room_join(company_id, student_id);
            send_reply(fd, REP_FAIL, user->id, "No free slot available. Candidate returned to waiting room queue.");
            break;
        }
        case REQ_HR_EXTEND_INTERVIEW: {
            int company_id = user->company_id > 0 ? user->company_id : req->company_id;
            int delay = 10;
            if (req->payload[0] != '\0') {
                int parsed = 0;
                if (sscanf(req->payload, "%d", &parsed) == 1 && parsed > 0 && parsed <= 120) {
                    delay = parsed;
                }
            }

            int affected = shift_overrun_and_queue_alerts(ctx, company_id, req->round_id, req->slot_id, delay);
            if (affected >= 0) {
                kill(getpid(), SIGUSR2);
                ipc_save_state(ctx);
                char msg[256];
                snprintf(msg,
                         sizeof(msg),
                         "Interview extended by %d min. Shifted subsequent slots. Alerts queued for %d students.",
                         delay,
                         affected);
                send_reply(fd, REP_SUCCESS, user->id, msg);
            } else {
                send_reply(fd, REP_FAIL, user->id, "Failed to shift schedule for overrun.");
            }
            break;
        }
        default:
            send_reply(fd, REP_FAIL, user->id, "Unsupported HR command.");
            break;
    }
}

static void handle_admin_request(ServerContext *ctx, int fd, const UserRecord *user, const NetworkPacket *req) {
    (void)user;

    switch (req->type) {
        case REQ_VIEW: {
            char summary[256] = {0};
            render_view_summary(ctx, summary, sizeof(summary));
            send_reply(fd, REP_SUCCESS, user->id, summary);
            break;
        }
        case REQ_ADMIN_BAN: {
            if (auth_set_ban_count(req->target_student_id, 3) == 0) {
                char line[128];
                snprintf(line, sizeof(line), "ADMIN_BAN student=%d", req->target_student_id);
                auth_append_audit("ADMIN_BAN", line);
                send_reply(fd, REP_SUCCESS, user->id, "Student banned.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Ban operation failed.");
            }
            break;
        }
        case REQ_ADMIN_FORCE_CANCEL: {
            if (slot_manager_cancel(ctx,
                                    req->target_student_id,
                                    req->company_id,
                                    req->round_id,
                                    req->slot_id,
                                    1) == 0) {
                send_reply(fd, REP_SUCCESS, user->id, "Force-cancel completed.");
            } else {
                send_reply(fd, REP_FAIL, user->id, "Force-cancel failed.");
            }
            break;
        }
        default:
            send_reply(fd, REP_FAIL, user->id, "Unsupported admin command.");
            break;
    }
}

static void *client_handler(void *arg) {
    HandlerArgs *h = (HandlerArgs *)arg;
    ServerContext *ctx = h->ctx;
    int fd = h->client_fd;

    free(h);

    while (ctx->running) {
        NetworkPacket req;
        UserRecord user;
        int rc = recv_packet(fd, &req);
        if (rc != 0) {
            break;
        }

        if (auth_lookup_user_by_id(req.user_id, &user) != 0) {
            send_reply(fd, REP_FAIL, req.user_id, "Invalid user id.");
            continue;
        }

        if (user.role == ROLE_STUDENT) {
            handle_student_request(ctx, fd, &user, &req);
        } else if (user.role == ROLE_HR) {
            handle_hr_request(ctx, fd, &user, &req);
        } else if (user.role == ROLE_ADMIN) {
            handle_admin_request(ctx, fd, &user, &req);
        } else {
            send_reply(fd, REP_FAIL, req.user_id, "Unknown role.");
        }
    }

    close(fd);
    decrease_active_connection_count(ctx);
    return NULL;
}

static int setup_listener(ServerContext *ctx) {
    struct sockaddr_in addr;
    int opt = 1;

    ctx->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->server_fd < 0) {
        return -1;
    }

    setsockopt(ctx->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -1;
    }

    if (listen(ctx->server_fd, SERVER_BACKLOG) != 0) {
        return -1;
    }

    return 0;
}

int main(void) {
    if (ipc_initialize(&g_ctx, 1) != 0) {
        fprintf(stderr, "Failed to initialize IPC resources.\n");
        return 1;
    }

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "Failed to install signal handlers.\n");
        ipc_cleanup(&g_ctx, 0);
        return 1;
    }

    if (round_engine_init() != 0) {
        fprintf(stderr, "Failed to initialize round engine.\n");
        ipc_cleanup(&g_ctx, 0);
        return 1;
    }

    if (waiting_room_init() != 0) {
        fprintf(stderr, "Failed to initialize waiting room dispatcher.\n");
        round_engine_shutdown();
        ipc_cleanup(&g_ctx, 0);
        return 1;
    }

    timer_manager_init(&g_ctx);

    if (setup_listener(&g_ctx) != 0) {
        perror("listen setup");
        round_engine_shutdown();
        ipc_cleanup(&g_ctx, 0);
        return 1;
    }

    printf("Placement server listening on port %d\n", SERVER_PORT);

    while (!g_stop_requested) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        if (g_overrun_broadcast_requested) {
            g_overrun_broadcast_requested = 0;
            process_overrun_alerts(&g_ctx);
        }

        client_fd = accept(g_ctx.server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!g_stop_requested) {
                perror("accept");
            }
            break;
        }

        if (sem_trywait(g_ctx.rate_sem) != 0) {
            if (errno == EAGAIN) {
                send_reply(client_fd, REP_SERVER_FULL, 0, "Server at capacity. Try again later.");
            } else {
                send_reply(client_fd, REP_FAIL, 0, "Server semaphore error.");
            }
            close(client_fd);
            continue;
        }

        increase_active_connection_count(&g_ctx);

        HandlerArgs *args = calloc(1, sizeof(*args));
        if (!args) {
            send_reply(client_fd, REP_FAIL, 0, "Server memory pressure. Retry.");
            close(client_fd);
            decrease_active_connection_count(&g_ctx);
            continue;
        }

        args->ctx = &g_ctx;
        args->client_fd = client_fd;
        args->addr = client_addr;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, args) != 0) {
            send_reply(client_fd, REP_FAIL, 0, "Failed to create handler thread.");
            close(client_fd);
            decrease_active_connection_count(&g_ctx);
            free(args);
            continue;
        }
        pthread_detach(tid);
    }

    g_ctx.running = 0;
    waiting_room_wake_all();

    ipc_save_state(&g_ctx);
    timer_manager_shutdown(&g_ctx);
    round_engine_shutdown();
    waiting_room_shutdown();
    outcome_tracker_sync_full(&g_ctx);
    ipc_cleanup(&g_ctx, 0);

    printf("Server shutdown complete.\n");
    return 0;
}
