#include "../common/models.h"

#include <ctype.h>

// locks audit file for safe reads
static int lock_fd(int fd, short lock_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}

// case-insensitive substring check
static int contains_case_insensitive(const char *haystack, const char *needle) {
    size_t hlen;
    size_t nlen;

    if (!needle || needle[0] == '\0') {
        return 1;
    }

    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (nlen > hlen) {
        return 0;
    }

    for (size_t i = 0; i <= hlen - nlen; ++i) {
        size_t j = 0;
        while (j < nlen) {
            char a = (char)tolower((unsigned char)haystack[i + j]);
            char b = (char)tolower((unsigned char)needle[j]);
            if (a != b) {
                break;
            }
            j++;
        }
        if (j == nlen) {
            return 1;
        }
    }

    return 0;
}

// audit log viewer entry
int main(int argc, char **argv) {
    const char *keyword = NULL;
    int fd;
    FILE *fp;
    char line[1024];
    int matched = 0;

    if (argc > 1) {
        keyword = argv[1];
    }

    fd = open(AUDIT_LOG_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open audit");
        return 1;
    }

    if (lock_fd(fd, F_RDLCK) != 0) {
        perror("lock audit");
        close(fd);
        return 1;
    }

    fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return 1;
    }

    printf("=== Placement Audit Viewer ===\n");
    if (keyword) {
        printf("Filter: %s\n\n", keyword);
    }

    while (fgets(line, sizeof(line), fp)) {
        if (contains_case_insensitive(line, keyword)) {
            fputs(line, stdout);
            matched = 1;
        }
    }

    if (!matched) {
        printf("No matching log entries.\n");
    }

    fclose(fp);
    return 0;
}
