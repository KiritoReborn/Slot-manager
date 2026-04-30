#include "../common/models.h"

typedef struct {
    int queue[WAITING_ROOM_CAPACITY];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} WaitingRoom;

static WaitingRoom g_rooms[MAX_COMPANIES];
static int g_waiting_room_initialized = 0;

static int queue_contains(const WaitingRoom *room, int student_id) {
    for (int i = 0; i < room->count; ++i) {
        int idx = (room->head + i) % WAITING_ROOM_CAPACITY;
        if (room->queue[idx] == student_id) {
            return 1;
        }
    }
    return 0;
}

int waiting_room_init(void) {
    if (g_waiting_room_initialized) {
        return 0;
    }

    for (int i = 0; i < MAX_COMPANIES; ++i) {
        WaitingRoom *room = &g_rooms[i];
        memset(room, 0, sizeof(*room));
        if (pthread_mutex_init(&room->lock, NULL) != 0) {
            return -1;
        }
        if (pthread_cond_init(&room->cond, NULL) != 0) {
            return -1;
        }
    }

    g_waiting_room_initialized = 1;
    return 0;
}

void waiting_room_shutdown(void) {
    if (!g_waiting_room_initialized) {
        return;
    }

    for (int i = 0; i < MAX_COMPANIES; ++i) {
        pthread_mutex_destroy(&g_rooms[i].lock);
        pthread_cond_destroy(&g_rooms[i].cond);
    }

    g_waiting_room_initialized = 0;
}

void waiting_room_wake_all(void) {
    if (!g_waiting_room_initialized) {
        return;
    }

    for (int i = 0; i < MAX_COMPANIES; ++i) {
        pthread_mutex_lock(&g_rooms[i].lock);
        pthread_cond_broadcast(&g_rooms[i].cond);
        pthread_mutex_unlock(&g_rooms[i].lock);
    }
}

int waiting_room_join(int company_id, int student_id) {
    WaitingRoom *room;

    if (!g_waiting_room_initialized || company_id <= 0 || company_id > MAX_COMPANIES || student_id <= 0) {
        errno = EINVAL;
        return -1;
    }

    room = &g_rooms[company_id - 1];

    pthread_mutex_lock(&room->lock);

    if (queue_contains(room, student_id)) {
        pthread_mutex_unlock(&room->lock);
        errno = EALREADY;
        return -1;
    }

    if (room->count >= WAITING_ROOM_CAPACITY) {
        pthread_mutex_unlock(&room->lock);
        errno = ENOSPC;
        return -1;
    }

    room->queue[room->tail] = student_id;
    room->tail = (room->tail + 1) % WAITING_ROOM_CAPACITY;
    room->count += 1;

    pthread_cond_signal(&room->cond);
    pthread_mutex_unlock(&room->lock);

    return 0;
}

int waiting_room_try_next_candidate(int company_id, int *out_student_id) {
    WaitingRoom *room;

    if (!g_waiting_room_initialized || company_id <= 0 || company_id > MAX_COMPANIES || !out_student_id) {
        errno = EINVAL;
        return -1;
    }

    room = &g_rooms[company_id - 1];
    pthread_mutex_lock(&room->lock);

    if (room->count == 0) {
        pthread_mutex_unlock(&room->lock);
        errno = EAGAIN;
        return -1;
    }

    *out_student_id = room->queue[room->head];
    room->head = (room->head + 1) % WAITING_ROOM_CAPACITY;
    room->count -= 1;

    pthread_mutex_unlock(&room->lock);
    return 0;
}

int waiting_room_next_candidate(int company_id, int *out_student_id, volatile sig_atomic_t *running_flag) {
    WaitingRoom *room;

    if (!g_waiting_room_initialized || company_id <= 0 || company_id > MAX_COMPANIES || !out_student_id) {
        errno = EINVAL;
        return -1;
    }

    room = &g_rooms[company_id - 1];

    pthread_mutex_lock(&room->lock);

    while (room->count == 0) {
        if (running_flag && !(*running_flag)) {
            pthread_mutex_unlock(&room->lock);
            errno = EINTR;
            return -1;
        }
        pthread_cond_wait(&room->cond, &room->lock);
        if (running_flag && !(*running_flag) && room->count == 0) {
            pthread_mutex_unlock(&room->lock);
            errno = EINTR;
            return -1;
        }
    }

    *out_student_id = room->queue[room->head];
    room->head = (room->head + 1) % WAITING_ROOM_CAPACITY;
    room->count -= 1;

    pthread_mutex_unlock(&room->lock);
    return 0;
}

int waiting_room_get_count(int company_id, int *out_count) {
    WaitingRoom *room;

    if (!g_waiting_room_initialized || company_id <= 0 || company_id > MAX_COMPANIES || !out_count) {
        errno = EINVAL;
        return -1;
    }

    room = &g_rooms[company_id - 1];
    pthread_mutex_lock(&room->lock);
    *out_count = room->count;
    pthread_mutex_unlock(&room->lock);

    return 0;
}
