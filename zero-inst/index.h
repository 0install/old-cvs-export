struct _Index {
	Element *doc;
	int ref;
};

Index *parse_index(const char *pathname, int validate, const char *site);
void index_foreach(Element *dir,
		   void (*fn)(Element *item, void *data),
		   void *data);
void index_free(Index *index);
Element *index_lookup(Index *index, const char *path);
Element *index_find_archive(Element *file);

void index_free(Index *site);

Element *index_get_root(Index *index);
