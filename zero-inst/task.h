extern Task *all_tasks;

typedef enum {
	TASK_KERNEL,	/* Handles a request from the kernel */
	TASK_CLIENT,	/* Handles a request from 0refresh or similar */
	TASK_INDEX,	/* Fetches a site index */
	TASK_ARCHIVE,	/* Fetches an archive */
} TaskType;

Task *task_new(TaskType type);
void task_destroy(Task *task, const char *error);
void task_process_done(pid_t pid, int success);
void task_set_string(Task *task, const char *str);
void task_set_index(Task *task, Index *index);
void task_steal_index(Task *task, Index *index);
void task_set_message(Task *task, DBusConnection *connection,
			DBusMessage *message);

struct _Task {
	TaskType type;
	int n;

	Task	*child_task;	/* A task that must finish first, or NULL */
	pid_t	child_pid;	/* A process that must finish first, or -1 */

	/* A callback to call when something happens.
	 * err == NULL on success.
	 */
	void (*step)(Task *task, const char *err);

	/* Various bits of extra data, dependant on 'type' */
	DBusConnection *connection;
	DBusMessage *message;

	void *data;
	uid_t uid;
	int fd;
	char *str;		/* Will be free()d */
	Index *index;		/* Will be unref'd */
	long size;

	int notify_on_end;

	Task	*next;		/* In all_tasks */
};
