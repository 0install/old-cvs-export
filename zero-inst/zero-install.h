#define MNT_DIR "/uri/0install"
#define META ".0inst-meta"

#define MAX_PATH_LEN 4096
#define MAX_URI_LEN 4096

extern char cache_dir[];
extern int cache_dir_len;	/* strlen(cache_dir) */

void kernel_cancel_task(/*@dependent@*/ Task *task);
