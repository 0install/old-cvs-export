#include <syslog.h>

#define error(x...) syslog(LOG_ERR, x)

void *my_malloc(size_t size);
void *my_realloc(void *old, size_t size);
char *my_strdup(const char *str);
void set_blocking(int fd, int blocking);
int ensure_dir(const char *path);
void close_on_exec(int fd, int close);
int check_md5(const char *path, const char *md5);
char *build_string(const char *format, ...);
