struct _Index {
	xmlDoc *doc;
	int ref;
};

Index *parse_index(const char *pathname, int validate, const char *site);
void index_foreach(xmlNode *dir,
		   void (*fn)(xmlNode *item, void *data),
		   void *data);
void index_free(Index *index);
xmlNode *index_lookup(Index *index, const char *path);
xmlNode *index_find_archive(xmlNode *file);

void index_free(Index *site);
void index_dump(Index *site);

void index_init(void);
void index_shutdown(void);
xmlNode *index_get_root(Index *index);
