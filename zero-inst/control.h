void create_control_socket(void);
int control_add_select(int n, fd_set *rfds, fd_set *wfds);
void control_check_select(fd_set *rfds, fd_set *wfds);
void control_drop_clients(void);
void control_push_updates(void);

void control_notify_update(Task *task);
void control_notify_end(Task *task);
void control_notify_error(Task *task, const char *message);
void control_cancel_task(Task *task);
