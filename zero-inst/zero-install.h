#define URI "/uri"
#define ZERO_INST_INDEX ".0inst.xml"
#define MAX_PATH_LEN 4096

extern char cache_dir[];

typedef struct _Request Request;
typedef struct _UserRequest UserRequest;

int queue_request(const char *path, const char *leaf, uid_t uid, int fd);

/* A Request begins in the READY state.
 *
 * It will then be quickly moved to FETCHING_INDEX. When the child process
 * completes, it looks at the index to work out which archive it needs and
 * moves to FETCHING_ARCHIVE while starting the download.
 * 
 * When that process completes, it finds the archive for the next file
 * request and fetches that, or removes the Request if there are no more.
 */

typedef enum {READY, FETCHING_INDEX, FETCHING_ARCHIVE} State;

struct _Request {
	char *path;

	int n_users;
	UserRequest *users;

	pid_t child_pid;

	Request *next;	/* link in open_requests */

	State state;
};

struct _UserRequest {
	int fd;		/* -1 => not from kernel */
	uid_t uid;

	char *leaf;
};

extern Request *open_requests;
