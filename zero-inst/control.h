int create_control_socket(void);
void read_from_control(int control);
int control_add_select(int n, fd_set *rfds, fd_set *wfds);
void control_check_select(fd_set *rfds, fd_set *wfds);
void control_notify_user(uid_t uid);
