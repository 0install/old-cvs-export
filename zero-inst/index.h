struct _Item {
	char type;	/* Dir, File, Link, eXec, Archive */
	char *leafname;
	char *target;
	char *md5;

	off_t size;
	time_t mtime;

	Item *next;
};

struct _Group {
	Item	*archives;
	Item	*items;

	Group	*next;
};

Index *parse_index(const char *path);
void index_foreach(Index *index,
		   void (*fn)(Item *item, void *data),
		   void *data);
void index_free(Index *index);
void index_dump(Index *index);
void index_lookup(Index *index, const char *leaf,
		  Group **group_ret, Item **item_ret);
