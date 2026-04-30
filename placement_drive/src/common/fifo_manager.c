#include "models.h"

int fifo_build_path(int student_id, char *out_path, size_t out_path_sz) {
    if (!out_path || out_path_sz == 0 || student_id <= 0) {
        errno = EINVAL;
        return -1;
    }

    int written = snprintf(out_path, out_path_sz, "/tmp/placement_notify_%d", student_id);
    if (written < 0 || (size_t)written >= out_path_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int fifo_create_for_student(int student_id) {
    char path[128];

    if (fifo_build_path(student_id, path, sizeof(path)) != 0) {
        return -1;
    }

    if (mkfifo(path, 0666) != 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }

    return 0;
}

int fifo_cleanup_for_student(int student_id) {
    char path[128];

    if (fifo_build_path(student_id, path, sizeof(path)) != 0) {
        return -1;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
}

int fifo_send_notification(int student_id, const NotifPacket *packet) {
    char path[128];
    int fd;

    if (!packet) {
        errno = EINVAL;
        return -1;
    }

    if (fifo_build_path(student_id, path, sizeof(path)) != 0) {
        return -1;
    }

    fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    ssize_t written = write(fd, packet, sizeof(*packet));
    close(fd);

    if (written != (ssize_t)sizeof(*packet)) {
        return -1;
    }

    return 0;
}
