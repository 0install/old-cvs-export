void create_control_socket(void);
int control_add_select(int n, fd_set *rfds, fd_set *wfds);
void control_check_select(fd_set *rfds, fd_set *wfds);
void control_notify_user(uid_t uid);
void control_drop_clients(void);
void control_push_updates(void);
