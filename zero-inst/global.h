#include <libxml/tree.h>

/* A task is an activity that may take a while to complete. Tasks may block
 * on other tasks.
 */
typedef struct _Task Task;
typedef struct _Index Index;

extern int verbose;
