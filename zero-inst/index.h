typedef struct _Index Index;

Index *parse_index(const char *path);
void index_free(Index *index);
void index_dump(Index *index);
