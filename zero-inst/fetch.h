Index *get_index(const char *path, Task **task, int force);
void fetch_create_directory(const char *path, xmlNode *node);
Task *fetch_archive(const char *file, xmlNode *archive, Index *index);
