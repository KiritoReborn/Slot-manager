#include "../common/models.h"

#include <arpa/inet.h>

typedef struct {
    PlacementState *state;
    OutcomeTable *outcomes;
    sem_t *rate_sem;
    volatile sig_atomic_t running;
} DashboardCtx;

typedef struct {
    int ok;
    NetworkPacket response;
} RequestResult;


// locks a file fd (read/write)
static int lock_fd(int fd, short lock_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fd, F_SETLKW, &fl);
}

// chops trailing newline from input
static void trim_newline(char *s) {
    if (!s) {
        return;
    }
    s[strcspn(s, "\n")] = '\0';
}

// asks user for a line and trims it
static int prompt_line(const char *label, char *out, size_t out_sz) {
    if (!label || !out || out_sz == 0) {
        return -1;
    }

    printf("%s", label);
    fflush(stdout);
    if (!fgets(out, (int)out_sz, stdin)) {
        return -1;
    }
    trim_newline(out);
    return 0;
}

// asks user for an int
static int prompt_int(const char *label, int *out_value) {
    char line[64];
    if (!out_value || prompt_line(label, line, sizeof(line)) != 0) {
        return -1;
    }
    if (sscanf(line, "%d", out_value) != 1) {
        return -1;
    }
    return 0;
}

// parses HH:MM into hours/mins
static int parse_hhmm(const char *text, int *hours, int *minutes) {
    int h;
    int m;
    if (!text || !hours || !minutes) {
        return -1;
    }
    if (sscanf(text, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    *hours = h;
    *minutes = m;
    return 0;
}


// builds a time window string from minutes
static void format_time_window_from_minutes(int start_minutes, int duration, char *out, size_t out_sz) {
    int end_minutes = start_minutes + duration;
    int sh = (start_minutes / 60) % 24;
    int sm = start_minutes % 60;
    int eh = (end_minutes / 60) % 24;
    int em = end_minutes % 60;
    snprintf(out, out_sz, "%02d:%02d-%02d:%02d", sh, sm, eh, em);
}

// parses a users.txt line into a struct
static int parse_user_line(const char *line, UserRecord *user) {
    char copy[512];
    char *token;
    char *save = NULL;

    if (!line || !user || line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
        return -1;
    }

    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    token = strtok_r(copy, ",\n", &save);
    if (!token) return -1;
    user->id = atoi(token);

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    strncpy(user->username, token, sizeof(user->username) - 1);
    user->username[sizeof(user->username) - 1] = '\0';

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    strncpy(user->password, token, sizeof(user->password) - 1);
    user->password[sizeof(user->password) - 1] = '\0';

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    user->role = role_from_string(token);

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    user->company_id = atoi(token);

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    user->cgpa = (float)atof(token);

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    user->ban_count = atoi(token);

    token = strtok_r(NULL, ",\n", &save);
    if (!token) return -1;
    user->daily_bookings = atoi(token);

    return 0;
}

// local login prompt + check in users file
static int local_login(UserRole role, UserRecord *out_user) {
    int fd;
    FILE *fp;
    char username[64];
    char password[64];
    char line[512];

    if (!out_user) {
        return -1;
    }

    if (prompt_line("Username: ", username, sizeof(username)) != 0) {
        return -1;
    }
    if (prompt_line("Password: ", password, sizeof(password)) != 0) {
        return -1;
    }

    fd = open(USERS_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open users");
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

        if (user.role == role && strcmp(user.username, username) == 0 && strcmp(user.password, password) == 0) {
            *out_user = user;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;
}

// loads all users from file into array
static int load_all_users(UserRecord *users, int max_users, int *out_count) {
    int fd;
    FILE *fp;
    char line[512];
    int count = 0;

    if (!users || max_users <= 0 || !out_count) {
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
        if (parse_user_line(line, &user) == 0) {
            if (count < max_users) {
                users[count++] = user;
            }
        }
    }

    fclose(fp);
    *out_count = count;
    return 0;
}

// writes user list back to file
static int save_all_users(const UserRecord *users, int count) {
    int fd;
    FILE *fp;

    if (!users || count < 0) {
        return -1;
    }

    fd = open(USERS_FILE, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if (lock_fd(fd, F_WRLCK) != 0) {
        close(fd);
        return -1;
    }

    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return -1;
    }

    fprintf(fp, "# id,username,password,role,company_id,cgpa,ban_count,daily_bookings\n");
    for (int i = 0; i < count; ++i) {
        fprintf(fp,
                "%d,%s,%s,%s,%d,%.1f,%d,%d\n",
                users[i].id,
                users[i].username,
                users[i].password,
                role_to_string(users[i].role),
                users[i].company_id,
                users[i].cgpa,
                users[i].ban_count,
                users[i].daily_bookings);
    }

    fflush(fp);
    fclose(fp);
    return 0;
}

// picks next user id
static int next_user_id(const UserRecord *users, int count) {
    int max_id = 0;
    for (int i = 0; i < count; ++i) {
        if (users[i].id > max_id) {
            max_id = users[i].id;
        }
    }
    return max_id + 1;
}

// finds user index by id
static int find_user_index_by_id(const UserRecord *users, int count, int id) {
    for (int i = 0; i < count; ++i) {
        if (users[i].id == id) {
            return i;
        }
    }
    return -1;
}

// prints full user details
static void print_full_user_info(const UserRecord *u) {
    if (!u) {
        return;
    }

    printf("ID            : %d\n", u->id);
    printf("Username      : %s\n", u->username);
    printf("Role          : %s\n", role_to_string(u->role));
    printf("Company ID    : %d\n", u->company_id);
    printf("CGPA          : %.2f\n", u->cgpa);
    printf("Ban Count     : %d\n", u->ban_count);
    printf("Daily Bookings: %d\n", u->daily_bookings);
}

// admin flow to add a user
static void admin_add_user(void) {
    UserRecord users[1024];
    int count = 0;
    UserRecord nu;
    int role_choice;
    char line[64];

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &count) != 0) {
        printf("Failed to load users.\n");
        return;
    }

    if (count >= (int)(sizeof(users) / sizeof(users[0]))) {
        printf("User list is full.\n");
        return;
    }

    memset(&nu, 0, sizeof(nu));
    nu.id = next_user_id(users, count);

    if (prompt_int("Role (1=STUDENT, 2=HR): ", &role_choice) != 0 || (role_choice != 1 && role_choice != 2)) {
        printf("Invalid role choice.\n");
        return;
    }
    nu.role = (role_choice == 1) ? ROLE_STUDENT : ROLE_HR;

    if (prompt_line("Username: ", nu.username, sizeof(nu.username)) != 0 || nu.username[0] == '\0') {
        printf("Invalid username.\n");
        return;
    }
    if (prompt_line("Password: ", nu.password, sizeof(nu.password)) != 0 || nu.password[0] == '\0') {
        printf("Invalid password.\n");
        return;
    }

    if (nu.role == ROLE_HR) {
        if (prompt_int("Company ID (1-10): ", &nu.company_id) != 0 || nu.company_id < 1 || nu.company_id > 10) {
            printf("Invalid company id.\n");
            return;
        }
        nu.cgpa = 0.0f;
    } else {
        if (prompt_line("CGPA (e.g., 8.7): ", line, sizeof(line)) != 0 || sscanf(line, "%f", &nu.cgpa) != 1) {
            printf("Invalid CGPA.\n");
            return;
        }
        nu.company_id = 0;
    }

    nu.ban_count = 0;
    nu.daily_bookings = 0;

    users[count++] = nu;
    if (save_all_users(users, count) != 0) {
        printf("Failed to save users.\n");
        return;
    }

    printf("User added successfully. New ID: %d\n", nu.id);
}

// admin flow to remove a user
static void admin_remove_user(void) {
    UserRecord users[1024];
    int count = 0;
    int id;
    int idx;

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &count) != 0) {
        printf("Failed to load users.\n");
        return;
    }

    if (prompt_int("User ID to remove: ", &id) != 0) {
        printf("Invalid id.\n");
        return;
    }

    idx = find_user_index_by_id(users, count, id);
    if (idx < 0) {
        printf("User not found.\n");
        return;
    }

    if (users[idx].role == ROLE_ADMIN) {
        printf("Admin users cannot be removed from this menu.\n");
        return;
    }

    for (int i = idx; i < count - 1; ++i) {
        users[i] = users[i + 1];
    }
    count--;

    if (save_all_users(users, count) != 0) {
        printf("Failed to save users.\n");
        return;
    }

    printf("User removed successfully.\n");
}

// admin flow to edit a user
static void admin_update_user(void) {
    UserRecord users[1024];
    int count = 0;
    int id;
    int idx;
    char line[128];

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &count) != 0) {
        printf("Failed to load users.\n");
        return;
    }

    if (prompt_int("User ID to update: ", &id) != 0) {
        printf("Invalid id.\n");
        return;
    }

    idx = find_user_index_by_id(users, count, id);
    if (idx < 0) {
        printf("User not found.\n");
        return;
    }

    if (!(users[idx].role == ROLE_STUDENT || users[idx].role == ROLE_HR)) {
        printf("Only STUDENT/HR update is supported here.\n");
        return;
    }

    printf("Current user details:\n");
    print_full_user_info(&users[idx]);

    if (prompt_line("New username (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        strncpy(users[idx].username, line, sizeof(users[idx].username) - 1);
        users[idx].username[sizeof(users[idx].username) - 1] = '\0';
    }
    if (prompt_line("New password (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        strncpy(users[idx].password, line, sizeof(users[idx].password) - 1);
        users[idx].password[sizeof(users[idx].password) - 1] = '\0';
    }
    if (prompt_line("New company id (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        int v;
        if (sscanf(line, "%d", &v) == 1) {
            users[idx].company_id = v;
        }
    }
    if (prompt_line("New CGPA (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        float v;
        if (sscanf(line, "%f", &v) == 1) {
            users[idx].cgpa = v;
        }
    }
    if (prompt_line("New ban count (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        int v;
        if (sscanf(line, "%d", &v) == 1) {
            users[idx].ban_count = v;
        }
    }
    if (prompt_line("New daily bookings (Enter to keep): ", line, sizeof(line)) == 0 && line[0] != '\0') {
        int v;
        if (sscanf(line, "%d", &v) == 1) {
            users[idx].daily_bookings = v;
        }
    }

    if (save_all_users(users, count) != 0) {
        printf("Failed to save users.\n");
        return;
    }

    printf("User updated successfully.\n");
}

// returns user ptr by id
static const UserRecord *find_user_by_id(const UserRecord *users, int count, int id) {
    for (int i = 0; i < count; ++i) {
        if (users[i].id == id) {
            return &users[i];
        }
    }
    return NULL;
}

// opens tcp connection to server
static int connect_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;

    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// sends req and waits for reply
static RequestResult send_request(int server_fd, NetworkPacket *request) {
    RequestResult rr;
    memset(&rr, 0, sizeof(rr));

    if (send_packet(server_fd, request) != 0) {
        printf("Request send failed.\n");
        return rr;
    }

    if (recv_packet(server_fd, &rr.response) != 0) {
        printf("Server disconnected.\n");
        return rr;
    }

    rr.ok = 1;
    return rr;
}

// sends req and prints reply
static int send_and_receive(int server_fd, NetworkPacket *request) {
    RequestResult rr = send_request(server_fd, request);
    if (!rr.ok) {
        return -1;
    }

    if (rr.response.type == REP_SUCCESS || rr.response.type == REP_ALERT || rr.response.type == REP_WAITLIST_PROMOTED) {
        printf("[OK] %s\n", rr.response.payload);
    } else if (rr.response.type == REP_SERVER_FULL) {
        printf("[SERVER FULL] %s\n", rr.response.payload);
    } else {
        printf("[FAIL] %s\n", rr.response.payload);
    }

    return 0;
}

// attaches shm state or warns
static PlacementState *attach_state_or_warn(void) {
    int shmid = -1;
    PlacementState *state = ipc_attach_state_ro(&shmid);
    if (!state) {
        printf("Server state unavailable. Start server first.\n");
        return NULL;
    }
    return state;
}

// prints quick company slot summary
static void print_companies_summary(const PlacementState *state) {
    printf("\n=== Company Slot Summary (Round 1) ===\n");
    printf("ID  Company         Filled/Total  Free\n");
    printf("---------------------------------------\n");

    for (int c = 0; c < MAX_COMPANIES; ++c) {
        const Company *company = &state->companies[c];
        if (company->company_id <= 0 || company->total_rounds <= 0) {
            continue;
        }
        const InterviewRound *round = &company->rounds[0];
        int filled = round->total_slots - round->available_slots;
        int free_slots = round->available_slots;

        printf("%-3d %-15s %-12d %-4d\n",
               company->company_id,
               company->company_name,
               filled,
               free_slots);
    }
    printf("---------------------------------------\n\n");
}

// prints a grid of slots for one company
static void print_company_slots_grid(const PlacementState *state, int company_id) {
    if (company_id <= 0 || company_id > MAX_COMPANIES) {
        printf("Invalid company id.\n");
        return;
    }

    const Company *company = &state->companies[company_id - 1];
    if (company->company_id <= 0 || company->total_rounds <= 0) {
        printf("Company not available.\n");
        return;
    }

    const InterviewRound *round = &company->rounds[0];

    printf("\n=== Slots for %s (Round 1) ===\n", company->company_name);
    for (int s = 0; s < round->total_slots; ++s) {
        const InterviewSlot *slot = &round->slots[s];
        printf("[%02d] %-11s %-7s",
               slot->slot_id,
               slot->time_window,
               slot->is_booked ? "BOOKED" : "FREE");
        if (slot->is_booked) {
            printf(" S%-3d", slot->booked_student_id);
        } else {
            printf(" -    ");
        }

        if ((s + 1) % 3 == 0 || s == round->total_slots - 1) {
            printf("\n");
        } else {
            printf(" | ");
        }
    }
    printf("\n");
}

// menu flow for slot map view
static void view_slot_map_scalable(void) {
    PlacementState *state = attach_state_or_warn();
    int company_id;
    if (!state) {
        return;
    }

    print_companies_summary(state);
    if (prompt_int("Enter company id to view detailed slots (0 to skip): ", &company_id) == 0 && company_id > 0) {
        print_company_slots_grid(state, company_id);
    }

    ipc_detach_state(state);
}

// shows bookings + waitlist for a student
static void print_student_own_bookings(int student_id) {
    PlacementState *state = attach_state_or_warn();
    int booked_found = 0;
    int waitlist_found = 0;

    if (!state) {
        return;
    }

    printf("\n=== My Current Bookings ===\n");
    for (int c = 0; c < MAX_COMPANIES; ++c) {
        Company *company = &state->companies[c];
        if (company->company_id <= 0 || company->total_rounds <= 0) {
            continue;
        }

        InterviewRound *round = &company->rounds[0];
        for (int s = 0; s < round->total_slots; ++s) {
            InterviewSlot *slot = &round->slots[s];
            if (slot->is_booked && slot->booked_student_id == student_id) {
                booked_found = 1;
                printf("Company=%s (ID=%d) Slot=%d Time=%s CheckIn=%s\n",
                       company->company_name,
                       company->company_id,
                       slot->slot_id,
                       slot->time_window,
                       slot->checked_in ? "YES" : "NO");
            }
        }
    }

    if (!booked_found) {
        printf("No active bookings found.\n");
    }

    printf("\n=== My Waitlist Queue Entries ===\n");
    for (int i = 0; i < state->waitlist_count; ++i) {
        WaitlistEntry *entry = &state->waitlist_entries[i];
        if (entry->student_id == student_id) {
            waitlist_found = 1;
            printf("CompanyID=%d Slot=%d CGPA=%.2f Priority=%ld\n",
                   entry->company_id,
                   entry->slot_id,
                   entry->cgpa,
                   entry->priority);
        }
    }
    if (!waitlist_found) {
        printf("No waitlist entries.\n");
    }

    printf("\n");
    ipc_detach_state(state);
}

// prints users by role
static void admin_print_users_by_role(UserRole role) {
    UserRecord users[1024];
    int count = 0;
    int total = 0;

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &count) != 0) {
        printf("Failed to read users.\n");
        return;
    }

    printf("\n=== %s List ===\n", role == ROLE_STUDENT ? "Student" : "HR");
    printf("ID  Username          Ban  DailyBookings  Company\n");
    printf("-----------------------------------------------\n");
    for (int i = 0; i < count; ++i) {
        if (users[i].role == role) {
            total++;
            printf("%-3d %-17s %-4d %-13d %-7d\n",
                   users[i].id,
                   users[i].username,
                   users[i].ban_count,
                   users[i].daily_bookings,
                   users[i].company_id);
        }
    }
    printf("Total %s: %d\n\n", role == ROLE_STUDENT ? "students" : "HRs", total);
}

// HR view of booked students
static void hr_view_students_and_slots(int hr_company_id) {
    UserRecord users[1024];
    int user_count = 0;
    PlacementState *state = attach_state_or_warn();
    int found = 0;

    if (!state) {
        return;
    }

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &user_count) != 0) {
        printf("Failed to read users file.\n");
        ipc_detach_state(state);
        return;
    }

    if (hr_company_id <= 0 || hr_company_id > MAX_COMPANIES) {
        printf("Invalid HR company mapping.\n");
        ipc_detach_state(state);
        return;
    }

    Company *company = &state->companies[hr_company_id - 1];
    if (company->company_id <= 0 || company->total_rounds <= 0) {
        printf("Company data unavailable.\n");
        ipc_detach_state(state);
        return;
    }

    InterviewRound *round = &company->rounds[0];

    printf("\n=== Roster: %s (Round 1) ===\n", company->company_name);
    printf("Slot  Time         StudentID  StudentName\n");
    printf("-----------------------------------------\n");

    for (int s = 0; s < round->total_slots; ++s) {
        InterviewSlot *slot = &round->slots[s];
        if (!slot->is_booked || slot->booked_student_id <= 0) {
            continue;
        }

        const UserRecord *u = find_user_by_id(users, user_count, slot->booked_student_id);
        const char *name = u ? u->username : "unknown";
        printf("%-5d %-12s %-10d %s\n", slot->slot_id, slot->time_window, slot->booked_student_id, name);
        found = 1;
    }

    if (!found) {
        printf("No students currently booked.\n");
    }
    printf("\n");

    ipc_detach_state(state);
}

// HR dashboard wrapper
static void hr_view_recruiter_dashboard(int hr_company_id) {
    PlacementState *state = attach_state_or_warn();
    if (state) {
        print_company_slots_grid(state, hr_company_id);
        ipc_detach_state(state);
    }
    hr_view_students_and_slots(hr_company_id);
}

// HR view for single student
static void hr_view_student_info_by_id(void) {
    UserRecord users[1024];
    int count = 0;
    int student_id;
    int idx;

    if (load_all_users(users, (int)(sizeof(users) / sizeof(users[0])), &count) != 0) {
        printf("Failed to load users.\n");
        return;
    }

    if (prompt_int("Student ID: ", &student_id) != 0) {
        printf("Invalid student id.\n");
        return;
    }

    idx = find_user_index_by_id(users, count, student_id);
    if (idx < 0 || users[idx].role != ROLE_STUDENT) {
        printf("Student not found.\n");
        return;
    }

    printf("\n=== Student Full Info ===\n");
    print_full_user_info(&users[idx]);
    printf("\n");
}

// HR batch add slots with time window
static void hr_add_slots_batch(int server_fd, const UserRecord *user) {
    char start_text[32];
    int sh;
    int sm;
    int duration;
    int count;
    int start_minutes;
    int ok_count = 0;

    if (prompt_line("Start time (HH:MM): ", start_text, sizeof(start_text)) != 0 ||
        parse_hhmm(start_text, &sh, &sm) != 0) {
        printf("Invalid start time format. Example: 09:30\n");
        return;
    }

    if (prompt_int("Duration per slot (minutes): ", &duration) != 0 || duration <= 0 || duration > 240) {
        printf("Invalid duration.\n");
        return;
    }

    if (prompt_int("Number of slots to create: ", &count) != 0 || count <= 0 || count > 20) {
        printf("Invalid slot count.\n");
        return;
    }

    start_minutes = sh * 60 + sm;

    for (int i = 0; i < count; ++i) {
        NetworkPacket req;
        char window[64];
        RequestResult rr;

        memset(&req, 0, sizeof(req));
        req.type = REQ_HR_ADD_SLOT;
        req.user_id = user->id;
        req.company_id = user->company_id;
        req.round_id = 0;

        format_time_window_from_minutes(start_minutes + i * duration, duration, window, sizeof(window));
        strncpy(req.payload, window, sizeof(req.payload) - 1);

        rr = send_request(server_fd, &req);
        if (!rr.ok) {
            printf("Stopped: lost server connection while adding slots.\n");
            break;
        }

        if (rr.response.type == REP_SUCCESS) {
            ok_count++;
            printf("[ADDED] %s\n", window);
        } else {
            printf("[SKIPPED] %s -> %s\n", window, rr.response.payload);
        }
    }

    printf("Batch add complete: %d/%d slots added.\n", ok_count, count);
}

// renders one dashboard frame
static void render_dashboard_once(DashboardCtx *ctx) {
    int sem_value = 0;
    int active_connections = 0;
    time_t now = time(NULL);
    struct tm tm_now;

    if (!ctx || !ctx->state || !ctx->outcomes || !ctx->rate_sem) {
        return;
    }

    sem_getvalue(ctx->rate_sem, &sem_value);
    active_connections = MAX_CONCURRENT_CONNECTIONS - sem_value;
    if (active_connections < 0) {
        active_connections = 0;
    }

    localtime_r(&now, &tm_now);

    printf("\033[2J\033[H");
    printf("=== MINI LIVE DASHBOARD ===\n");
    printf("Time: %04d-%02d-%02d %02d:%02d:%02d\n",
           tm_now.tm_year + 1900,
           tm_now.tm_mon + 1,
           tm_now.tm_mday,
           tm_now.tm_hour,
           tm_now.tm_min,
           tm_now.tm_sec);
    printf("Active connections: %d/%d\n\n", active_connections, MAX_CONCURRENT_CONNECTIONS);

    for (int c = 0; c < MAX_COMPANIES; ++c) {
        Company *company = &ctx->state->companies[c];
        if (company->company_id <= 0 || company->total_rounds <= 0) {
            continue;
        }

        InterviewRound *round = &company->rounds[0];
        int filled = round->total_slots - round->available_slots;
        int pass_count = 0;
        int fail_count = 0;

        for (int s = 0; s < MAX_STUDENTS; ++s) {
            InterviewOutcome out = ctx->outcomes->results[s][company->company_id - 1][0];
            if (out == OUTCOME_PASS) {
                pass_count++;
            } else if (out == OUTCOME_FAIL || out == OUTCOME_NO_SHOW) {
                fail_count++;
            }
        }

        printf("[%2d] %-12s  Slots: %2d/%2d  Pass: %-3d  Fail: %-3d\n",
               company->company_id,
               company->company_name,
               filled,
               round->total_slots,
               pass_count,
               fail_count);
    }

    printf("\nType q + Enter in this terminal to stop dashboard.\n");
    fflush(stdout);
}

// background loop to refresh dashboard
static void *dashboard_thread(void *arg) {
    DashboardCtx *ctx = (DashboardCtx *)arg;
    while (ctx->running) {
        render_dashboard_once(ctx);
        sleep(2);
    }
    return NULL;
}

// runs live dashboard until user quits
static void run_live_dashboard(void) {
    DashboardCtx ctx;
    pthread_t tid;
    int shmid = -1;
    int outcomes_fd;
    char line[16];

    memset(&ctx, 0, sizeof(ctx));

    ctx.state = ipc_attach_state_ro(&shmid);
    if (!ctx.state) {
        printf("Live dashboard unavailable. Start server first.\n");
        return;
    }

    outcomes_fd = open(OUTCOMES_FILE, O_RDONLY);
    if (outcomes_fd < 0) {
        printf("Cannot open outcomes file.\n");
        ipc_detach_state(ctx.state);
        return;
    }

    ctx.outcomes = mmap(NULL, sizeof(OutcomeTable), PROT_READ, MAP_SHARED, outcomes_fd, 0);
    if (ctx.outcomes == MAP_FAILED) {
        printf("Cannot map outcomes.\n");
        close(outcomes_fd);
        ipc_detach_state(ctx.state);
        return;
    }

    ctx.rate_sem = sem_open(RATE_SEM_NAME, 0);
    if (ctx.rate_sem == SEM_FAILED) {
        printf("Cannot open rate semaphore.\n");
        munmap(ctx.outcomes, sizeof(OutcomeTable));
        close(outcomes_fd);
        ipc_detach_state(ctx.state);
        return;
    }

    ctx.running = 1;
    if (pthread_create(&tid, NULL, dashboard_thread, &ctx) != 0) {
        printf("Failed to start dashboard thread.\n");
        sem_close(ctx.rate_sem);
        munmap(ctx.outcomes, sizeof(OutcomeTable));
        close(outcomes_fd);
        ipc_detach_state(ctx.state);
        return;
    }

    while (prompt_line("", line, sizeof(line)) == 0) {
        if (line[0] == 'q' || line[0] == 'Q') {
            break;
        }
    }

    ctx.running = 0;
    pthread_join(tid, NULL);

    sem_close(ctx.rate_sem);
    munmap(ctx.outcomes, sizeof(OutcomeTable));
    close(outcomes_fd);
    ipc_detach_state(ctx.state);

    printf("Dashboard closed.\n");
}

// prints last N audit log lines
static void print_audit_tail(int tail_lines) {
    FILE *fp;
    char ring[50][1024];
    int count = 0;
    int start = 0;
    char line[1024];

    if (tail_lines <= 0 || tail_lines > 50) {
        tail_lines = 20;
    }

    fp = fopen(AUDIT_LOG_FILE, "r");
    if (!fp) {
        printf("Could not open audit log.\n");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        strncpy(ring[count % tail_lines], line, sizeof(ring[0]) - 1);
        ring[count % tail_lines][sizeof(ring[0]) - 1] = '\0';
        count++;
    }
    fclose(fp);

    if (count > tail_lines) {
        start = count - tail_lines;
    }

    printf("\n=== Last %d Audit Lines ===\n", tail_lines);
    for (int i = start; i < count; ++i) {
        fputs(ring[i % tail_lines], stdout);
    }
    printf("===========================\n\n");
}

// student menu loop
static void student_menu(int server_fd, const UserRecord *user) {
    while (1) {
        int choice;
        NetworkPacket req;
        memset(&req, 0, sizeof(req));
        req.user_id = user->id;
        req.round_id = 0;

        printf("\n--- Student Menu ---\n");
        printf("1. View Slot Map\n");
        printf("2. View My Bookings\n");
        printf("3. Join Waiting Room Queue\n");
        printf("4. Book Slot\n");
        printf("5. Cancel Slot\n");
        printf("6. Check-In\n");
        printf("7. Back\n");

        if (prompt_int("Choice: ", &choice) != 0) {
            printf("Invalid input.\n");
            continue;
        }

        if (choice == 7) {
            break;
        }

        if (choice == 1) {
            view_slot_map_scalable();
            continue;
        }

        if (choice == 2) {
            print_student_own_bookings(user->id);
            continue;
        }

        if (choice == 3) {
            req.type = REQ_JOIN_WAITING_ROOM;
            if (prompt_int("Company ID for waiting room: ", &req.company_id) != 0) {
                printf("Invalid company id.\n");
                continue;
            }
            send_and_receive(server_fd, &req);
            continue;
        }

        if (prompt_int("Company ID: ", &req.company_id) != 0 ||
            prompt_int("Slot ID (from map): ", &req.slot_id) != 0) {
            printf("Invalid company/slot input.\n");
            continue;
        }

        if (choice == 4) {
            req.type = REQ_BOOK;
        } else if (choice == 5) {
            req.type = REQ_CANCEL;
        } else if (choice == 6) {
            req.type = REQ_CHECKIN;
        } else {
            printf("Invalid choice.\n");
            continue;
        }

        send_and_receive(server_fd, &req);
    }
}

// HR menu loop
static void hr_menu(int server_fd, const UserRecord *user) {
    while (1) {
        int choice;
        NetworkPacket req;
        memset(&req, 0, sizeof(req));
        req.user_id = user->id;
        req.company_id = user->company_id;
        req.round_id = 0;

        printf("\n--- HR Menu ---\n");
        printf("1. Recruiter Dashboard (slots + assigned students)\n");
        printf("2. View Student Full Info by ID\n");
        printf("3. Add Multiple Slots (start + duration + count)\n");
        printf("4. Mark Outcome (Round 1)\n");
        printf("5. Dispatch Next Candidate (Waiting Room)\n");
        printf("6. Extend Current Interview (Shift next slots)\n");
        printf("7. Back\n");

        if (prompt_int("Choice: ", &choice) != 0) {
            printf("Invalid input.\n");
            continue;
        }

        if (choice == 7) {
            break;
        }

        if (choice == 1) {
            hr_view_recruiter_dashboard(user->company_id);
            continue;
        }

        if (choice == 2) {
            hr_view_student_info_by_id();
            continue;
        }

        if (choice == 3) {
            hr_add_slots_batch(server_fd, user);
            continue;
        }

        if (choice == 4) {
            req.type = REQ_HR_MARK_OUTCOME;
            if (prompt_int("Student ID: ", &req.target_student_id) != 0) {
                printf("Invalid student id.\n");
                continue;
            }
            if (prompt_line("Outcome (PASS/FAIL/HOLD/NO_SHOW): ", req.payload, sizeof(req.payload)) != 0) {
                printf("Invalid outcome.\n");
                continue;
            }
            send_and_receive(server_fd, &req);
            continue;
        }

        if (choice == 5) {
            req.type = REQ_HR_NEXT_CANDIDATE;
            send_and_receive(server_fd, &req);
            continue;
        }

        if (choice == 6) {
            int delay = 10;
            req.type = REQ_HR_EXTEND_INTERVIEW;
            if (prompt_int("Current running slot id: ", &req.slot_id) != 0) {
                printf("Invalid slot id.\n");
                continue;
            }
            if (prompt_int("Delay minutes (default 10): ", &delay) != 0) {
                delay = 10;
            }
            snprintf(req.payload, sizeof(req.payload), "%d", delay);
            send_and_receive(server_fd, &req);
            continue;
        }

        printf("Invalid choice.\n");
    }
}

// admin sub-menu for user mgmt
static void admin_manage_users_menu(void) {
    while (1) {
        int choice;
        printf("\n--- Admin User Management ---\n");
        printf("1. List All Students (+total)\n");
        printf("2. List All HRs (+total)\n");
        printf("3. Add Student/HR User\n");
        printf("4. Remove Student/HR User\n");
        printf("5. Update Student/HR User\n");
        printf("6. Back\n");

        if (prompt_int("Choice: ", &choice) != 0) {
            printf("Invalid input.\n");
            continue;
        }

        if (choice == 6) {
            break;
        }

        if (choice == 1) {
            admin_print_users_by_role(ROLE_STUDENT);
            continue;
        }

        if (choice == 2) {
            admin_print_users_by_role(ROLE_HR);
            continue;
        }

        if (choice == 3) {
            admin_add_user();
            continue;
        }

        if (choice == 4) {
            admin_remove_user();
            continue;
        }

        if (choice == 5) {
            admin_update_user();
            continue;
        }

        printf("Invalid choice.\n");
    }
}

// admin menu loop
static void admin_menu(int server_fd, const UserRecord *user) {
    while (1) {
        int choice;
        NetworkPacket req;
        memset(&req, 0, sizeof(req));
        req.user_id = user->id;
        req.round_id = 0;

        printf("\n--- Admin Menu ---\n");
        printf("1. View Slot Map (scalable)\n");
        printf("2. Manage Users\n");
        printf("3. Ban Student\n");
        printf("4. Force Cancel Slot\n");
        printf("5. View Audit Tail\n");
        printf("6. Back\n");

        if (prompt_int("Choice: ", &choice) != 0) {
            printf("Invalid input.\n");
            continue;
        }

        if (choice == 6) {
            break;
        }

        if (choice == 1) {
            view_slot_map_scalable();
            continue;
        }

        if (choice == 2) {
            admin_manage_users_menu();
            continue;
        }

        if (choice == 3) {
            req.type = REQ_ADMIN_BAN;
            if (prompt_int("Student ID to ban: ", &req.target_student_id) != 0) {
                printf("Invalid student id.\n");
                continue;
            }
            send_and_receive(server_fd, &req);
            continue;
        }

        if (choice == 4) {
            req.type = REQ_ADMIN_FORCE_CANCEL;
            if (prompt_int("Target student ID: ", &req.target_student_id) != 0 ||
                prompt_int("Company ID: ", &req.company_id) != 0 ||
                prompt_int("Slot ID: ", &req.slot_id) != 0) {
                printf("Invalid force-cancel input.\n");
                continue;
            }
            send_and_receive(server_fd, &req);
            continue;
        }

        if (choice == 5) {
            print_audit_tail(20);
            continue;
        }

        printf("Invalid choice.\n");
    }
}

// login + session for a role
static void role_session(UserRole role) {
    UserRecord user;
    int server_fd;

    if (local_login(role, &user) != 0) {
        printf("Login failed.\n");
        return;
    }

    server_fd = connect_server();
    if (server_fd < 0) {
        printf("Could not connect to server on 127.0.0.1:%d\n", SERVER_PORT);
        return;
    }

    if (role == ROLE_STUDENT) {
        student_menu(server_fd, &user);
    } else if (role == ROLE_HR) {
        hr_menu(server_fd, &user);
    } else {
        admin_menu(server_fd, &user);
    }

    close(server_fd);
}

// main client entry
int main(void) {
    printf("=== Campus Placement Mini Client ===\n");

    while (1) {
        int choice;

        printf("\nMain Menu\n");
        printf("1. Login as Student\n");
        printf("2. Login as HR\n");
        printf("3. Login as Admin\n");
        printf("4. Live Dashboard\n");
        printf("5. Exit\n");

        if (prompt_int("Choice: ", &choice) != 0) {
            printf("Invalid input.\n");
            continue;
        }

        if (choice == 1) {
            role_session(ROLE_STUDENT);
        } else if (choice == 2) {
            role_session(ROLE_HR);
        } else if (choice == 3) {
            role_session(ROLE_ADMIN);
        } else if (choice == 4) {
            run_live_dashboard();
        } else if (choice == 5) {
            break;
        } else {
            printf("Invalid choice.\n");
        }
    }

    printf("Client closed.\n");
    return 0;
}
