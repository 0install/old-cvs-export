struct _Element {
	char *name;
	Element *lastChild;
	Element *nextSibling;
	Element *previousSibling;
	Element *parentNode;
	char **attrs;
};

/*@owned@*/ Element *xml_new(const char *namespace, const char *pathname);
void xml_destroy(/*@owned@*/ Element *root);
/*@observer@*/ const char *xml_get_attr(Element *node, const char *name);
void xml_destroy_node(Element *node);
Element *xml_new_with_attrs(const char *name, const char **attrs);
void xml_add_child(Element *parent, Element *new);
