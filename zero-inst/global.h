#include "mydbus.h"

#include <stdio.h>

#include <interface.h>

/* A task is an activity that may take a while to complete. Tasks may block
 * on other tasks.
 */
typedef struct _Task Task;
typedef struct _Element Element;
typedef struct _Index Index;
typedef /*@refcounted@*/ Index *IndexP;

extern int copy_stderr;
extern int verbose;
