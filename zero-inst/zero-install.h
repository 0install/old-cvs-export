#define META ".0inst-meta"

#define MAX_PATH_LEN 4096
#define MAX_URI_LEN 4096

extern const char *mnt_dir;
extern int mnt_dir_len;

extern char cache_dir[];
extern int cache_dir_len;	/* strlen(cache_dir) */

void kernel_cancel_task(Task *task);
