void *my_malloc(size_t size);
void *my_realloc(void *old, size_t size);
char *my_strdup(const char *str);
void set_blocking(int fd, int blocking);
int build_uri(char *buffer, int len, const char *path,
		     const char *leaf1, const char *leaf2);
int uri_ensure_absolute(const char *uri, const char *base,
			char *result, int result_len);
int ensure_dir(const char *path);
void close_on_exec(int fd, int close);
