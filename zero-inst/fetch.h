Index *get_index(const char *path, Task **task, int force);
void fetch_create_directory(const char *path, Element *node);
Task *fetch_archive(const char *file, Element *group, Index *index);
int build_ddds_for_site(Index *index, const char *site);
void fetch_run_tests(void);
void fetch_set_auto_reject(const char *request, uid_t uid);
int fetch_check_auto_reject(const char *request, uid_t uid);
void fetch_init(void);
