struct _Index {
	Element *doc;
	char *site;
	/*@refs@*/ int ref;
};

IndexP parse_index(const char *pathname, int validate, const char *site);
void index_foreach(Element *dir,
		   void (*fn)(Element *item, void *data),
		   void *data);
void index_free(Index *index);
/*@dependent@*/ Element *index_lookup(Index *index, const char *path);
Element *index_find_archive(Element *file);

void index_free(Index *site);

/*@dependent@*/ Element *index_get_root(Index *index);
