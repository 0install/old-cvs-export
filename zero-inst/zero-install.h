#define URI "/uri"
extern const char *cache_dir;

typedef struct _Request Request;
typedef struct _UserRequest UserRequest;

struct _Request {
	char *path;

	int n_users;
	UserRequest *users;

	pid_t child_pid;

	Request *next;	/* link in open_requests */
};

struct _UserRequest {
	int fd;
	uid_t uid;

	char *leaf;
};

extern Request *open_requests;
