#include <libxml/tree.h>

typedef struct _Item Item;
typedef struct _Group Group;

/* A task is an activity that may take a while to complete. Tasks may block
 * on other tasks.
 */
typedef struct _Task Task;
typedef xmlDoc Index;
