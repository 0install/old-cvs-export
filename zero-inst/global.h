#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>

#include <stdio.h>

#include <interface.h>

/* A task is an activity that may take a while to complete. Tasks may block
 * on other tasks.
 */
typedef struct _Task Task;
typedef struct _Index Index;
typedef struct _Element Element;

extern int verbose;
