Index *parse_index(const char *path);
void index_foreach(xmlNode *dir,
		   void (*fn)(xmlNode *item, void *data),
		   void *data);
void index_free(Index *index);
#if 0
void index_lookup(Index *index, const char *leaf,
		  Group **group_ret, Item **item_ret);
#endif

void index_free(Index *site);
void index_dump(Index *site);

void index_init(void);
void index_shutdown(void);
xmlNode *index_get_root(Index *index);
