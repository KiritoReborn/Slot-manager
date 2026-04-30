#include "../common/models.h"

static int lock_fd(int fd, short lock_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}

static int parse_user_line(const char *line, UserRecord *user) {
    char copy[512];
    char *token;
    char *saveptr = NULL;

    if (!line || !user) {
        return -1;
    }

    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
        return -1;
    }

    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    token = strtok_r(copy, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->id = atoi(token);

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    strncpy(user->username, token, sizeof(user->username) - 1);
    user->username[sizeof(user->username) - 1] = '\0';

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    strncpy(user->password, token, sizeof(user->password) - 1);
    user->password[sizeof(user->password) - 1] = '\0';

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->role = role_from_string(token);

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->company_id = atoi(token);

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->cgpa = (float)atof(token);

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->ban_count = atoi(token);

    token = strtok_r(NULL, ",\n", &saveptr);
    if (!token) {
        return -1;
    }
    user->daily_bookings = atoi(token);

    return 0;
}

static int format_user_line(const UserRecord *user, char *out, size_t out_sz) {
    int n;
    if (!user || !out || out_sz == 0) {
        return -1;
    }

    n = snprintf(out, out_sz, "%d,%s,%s,%s,%d,%.1f,%d,%d\n",
                 user->id,
                 user->username,
                 user->password,
                 role_to_string(user->role),
                 user->company_id,
                 user->cgpa,
                 user->ban_count,
                 user->daily_bookings);

    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

static int append_text(char **buffer, size_t *len, size_t *cap, const char *text) {
    size_t add_len = strlen(text);
    if (*len + add_len + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 4096 : *cap * 2;
        while (new_cap < *len + add_len + 1) {
            new_cap *= 2;
        }
        char *new_buf = realloc(*buffer, new_cap);
        if (!new_buf) {
            return -1;
        }
        *buffer = new_buf;
        *cap = new_cap;
    }

    memcpy(*buffer + *len, text, add_len);
    *len += add_len;
    (*buffer)[*len] = '\0';
    return 0;
}

typedef struct {
    int delta;
    int new_value;
} DailyBookingMutatorArg;

static int update_user_record(int user_id, int (*mutator)(UserRecord *user, void *arg), void *arg, UserRecord *out_user) {
    int fd;
    FILE *fp = NULL;
    char line[512];
    char *new_content = NULL;
    size_t len = 0;
    size_t cap = 0;
    int found = 0;

    fd = open(USERS_FILE, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if (lock_fd(fd, F_WRLCK) != 0) {
        close(fd);
        return -1;
    }

    fp = fdopen(dup(fd), "r");
    if (!fp) {
        lock_fd(fd, F_UNLCK);
        close(fd);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        UserRecord user;
        if (parse_user_line(line, &user) != 0) {
            if (append_text(&new_content, &len, &cap, line) != 0) {
                fclose(fp);
                lock_fd(fd, F_UNLCK);
                close(fd);
                free(new_content);
                return -1;
            }
            continue;
        }

        if (user.id == user_id) {
            char formatted[512];
            found = 1;
            if (mutator && mutator(&user, arg) != 0) {
                fclose(fp);
                lock_fd(fd, F_UNLCK);
                close(fd);
                free(new_content);
                return -1;
            }
            if (out_user) {
                *out_user = user;
            }
            if (format_user_line(&user, formatted, sizeof(formatted)) != 0) {
                fclose(fp);
                lock_fd(fd, F_UNLCK);
                close(fd);
                free(new_content);
                return -1;
            }
            if (append_text(&new_content, &len, &cap, formatted) != 0) {
                fclose(fp);
                lock_fd(fd, F_UNLCK);
                close(fd);
                free(new_content);
                return -1;
            }
        } else {
            if (append_text(&new_content, &len, &cap, line) != 0) {
                fclose(fp);
                lock_fd(fd, F_UNLCK);
                close(fd);
                free(new_content);
                return -1;
            }
        }
    }

    fclose(fp);

    if (!found) {
        lock_fd(fd, F_UNLCK);
        close(fd);
        free(new_content);
        errno = ENOENT;
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) != 0) {
        lock_fd(fd, F_UNLCK);
        close(fd);
        free(new_content);
        return -1;
    }

    if (len > 0 && write(fd, new_content, len) != (ssize_t)len) {
        lock_fd(fd, F_UNLCK);
        close(fd);
        free(new_content);
        return -1;
    }

    fsync(fd);
    lock_fd(fd, F_UNLCK);
    close(fd);
    free(new_content);
    return 0;
}

static int set_ban_mutator(UserRecord *user, void *arg) {
    int value = *(int *)arg;
    if (value < 0) {
        value = 0;
    }
    user->ban_count = value;
    return 0;
}

static int increment_no_show_mutator(UserRecord *user, void *arg) {
    int *new_count = (int *)arg;
    user->ban_count += 1;
    if (new_count) {
        *new_count = user->ban_count;
    }
    return 0;
}

static int update_daily_booking_mutator(UserRecord *user, void *arg) {
    DailyBookingMutatorArg *m = (DailyBookingMutatorArg *)arg;
    user->daily_bookings += m->delta;
    if (user->daily_bookings < 0) {
        user->daily_bookings = 0;
    }
    m->new_value = user->daily_bookings;
    return 0;
}

int auth_validate_user(const char *username, const char *password, UserRole expected_role, UserRecord *out_user) {
    int fd;
    FILE *fp;
    char line[512];

    if (!username || !password || !out_user) {
        errno = EINVAL;
        return -1;
    }

    fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (lock_fd(fd, F_RDLCK) != 0) {
        close(fd);
        return -1;
    }

    fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        UserRecord user;
        if (parse_user_line(line, &user) != 0) {
            continue;
        }

        if (strcmp(user.username, username) == 0 && strcmp(user.password, password) == 0 && user.role == expected_role) {
            *out_user = user;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    errno = EPERM;
    return -1;
}

int auth_lookup_user_by_id(int user_id, UserRecord *out_user) {
    int fd;
    FILE *fp;
    char line[512];

    if (user_id <= 0 || !out_user) {
        errno = EINVAL;
        return -1;
    }

    fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (lock_fd(fd, F_RDLCK) != 0) {
        close(fd);
        return -1;
    }

    fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        UserRecord user;
        if (parse_user_line(line, &user) == 0 && user.id == user_id) {
            *out_user = user;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    errno = ENOENT;
    return -1;
}

int auth_lookup_cgpa(int student_id, float *out_cgpa, char *out_name, size_t out_name_len) {
    UserRecord user;

    if (auth_lookup_user_by_id(student_id, &user) != 0) {
        return -1;
    }

    if (out_cgpa) {
        *out_cgpa = user.cgpa;
    }
    if (out_name && out_name_len > 0) {
        strncpy(out_name, user.username, out_name_len - 1);
        out_name[out_name_len - 1] = '\0';
    }

    return 0;
}

int auth_set_ban_count(int student_id, int ban_count) {
    return update_user_record(student_id, set_ban_mutator, &ban_count, NULL);
}

int auth_increment_no_show(int student_id, int *new_ban_count) {
    int temp = 0;
    int rc = update_user_record(student_id, increment_no_show_mutator, &temp, NULL);
    if (rc == 0 && new_ban_count) {
        *new_ban_count = temp;
    }
    return rc;
}

int auth_update_daily_bookings(int student_id, int delta, int *new_value) {
    DailyBookingMutatorArg arg;
    arg.delta = delta;
    arg.new_value = 0;

    int rc = update_user_record(student_id, update_daily_booking_mutator, &arg, NULL);
    if (rc == 0 && new_value) {
        *new_value = arg.new_value;
    }
    return rc;
}

int auth_is_banned(int student_id, int *is_banned) {
    UserRecord user;
    if (!is_banned) {
        errno = EINVAL;
        return -1;
    }

    if (auth_lookup_user_by_id(student_id, &user) != 0) {
        return -1;
    }

    *is_banned = (user.ban_count >= 3) ? 1 : 0;
    return 0;
}

int auth_append_audit(const char *event_name, const char *details) {
    int fd;
    char line[1024];
    struct flock fl;
    time_t now = time(NULL);
    struct tm tm_now;

    if (!event_name) {
        errno = EINVAL;
        return -1;
    }

    fd = open(AUDIT_LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) {
        return -1;
    }

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLKW, &fl) != 0) {
        close(fd);
        return -1;
    }

    localtime_r(&now, &tm_now);
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d | %-18s | %s\n",
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec,
             event_name,
             details ? details : "-");

    if (write(fd, line, strlen(line)) < 0) {
        fl.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &fl);
        close(fd);
        return -1;
    }

    fsync(fd);

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
    return 0;
}
