#define MNT_DIR "/uri/0http"
#define ZERO_INST_INDEX ".0inst.xml"
#define MAX_PATH_LEN 4096
#define MAX_URI_LEN 4096

extern char cache_dir[];

typedef struct _Request Request;
typedef struct _UserRequest UserRequest;

int queue_request(const char *path, const char *leaf, uid_t uid, int fd);

/* State meanings on entry to request_next_step(): */
typedef enum {
	READY,		/* Request just started - fetch index */
	FETCHING_INDEX,	/* Index fetched - process and fetch archive */
	FETCHING_SUBINDEX,	/* Subindex fetched - create directory */
	FETCHING_ARCHIVE,	/* Archive fetched - extract files */
} State;

#if 0
struct _Request {
	char *path;
	Index *index;

	int n_users;
	UserRequest *users;

	pid_t child_pid;

	Request *next;	/* link in open_requests */

	State state;

	/* This is the location of the file that wget is currently
	 * fetching. Also used by 0show to measure progress.
	 */
	char *current_download_path;
	Item *current_download_archive; /* Pointer into ->index */
};

struct _UserRequest {
	int fd;		/* -1 => not from kernel */
	uid_t uid;

	char *leaf;
};
#endif

extern Request *open_requests;
