#ifdef S_SPLINT_S
/* Taken from the LGPL'd DBUS header files. Splint annotations have been added to
 * allow for better static checking.
 */

#ifndef MY_DBUS_H
#define MY_DBUS_H

#define DBUS_INSIDE_DBUS_H 1

#include <dbus/dbus-macros.h>
#include <dbus/dbus-types.h>
#include <dbus/dbus-memory.h>

#define DBUS_ERROR_H
#define DBUS_CONNECTION_H

typedef struct DBusError DBusError;
void dbus_error_init(/*@out@*/ DBusError *error);
void dbus_error_free(DBusError *error);
dbus_bool_t dbus_error_is_set(const DBusError *error);
typedef struct DBusMessage DBusMessage;
typedef /*@refcounted@*/ DBusMessage *pDBusMessage;

typedef struct DBusWatch DBusWatch;
typedef struct DBusTimeout DBusTimeout;
typedef struct DBusPreallocatedSend DBusPreallocatedSend;
typedef struct DBusPendingCall DBusPendingCall;
typedef struct DBusConnection DBusConnection;
typedef struct DBusObjectPathVTable DBusObjectPathVTable;

typedef /*@refcounted@*/ DBusConnection *pDBusConnection;

typedef enum
{
  DBUS_DISPATCH_DATA_REMAINS,  /**< There is more data to potentially convert to messages. */
  DBUS_DISPATCH_COMPLETE,      /**< All currently available data has been processed. */
  DBUS_DISPATCH_NEED_MEMORY    /**< More memory is needed to continue. */
} DBusDispatchStatus;

typedef enum
{
  DBUS_HANDLER_RESULT_HANDLED,         /**< Message has had its effect */ 
  DBUS_HANDLER_RESULT_NOT_YET_HANDLED, /**< Message has not had any effect */
  DBUS_HANDLER_RESULT_NEED_MEMORY      /**< Need more memory to return another result */
} DBusHandlerResult;

pDBusMessage dbus_message_new(int message_type);
pDBusMessage dbus_message_new_method_call(/*@null@*/ const char  *service,
                                             const char  *path,
                                             const char  *interface,
                                             const char  *method);
pDBusMessage dbus_message_new_method_return (DBusMessage *method_call);
pDBusMessage dbus_message_new_error         (DBusMessage *reply_to,
                                             const char  *error_name,
                                             const char  *error_message);

//void          dbus_message_ref              (DBusMessage   *message);
void          dbus_message_unref            (/*@killref@*/ pDBusMessage   message);

typedef dbus_bool_t (* DBusAddWatchFunction)       (DBusWatch      *watch,
                                                    void           *data);
typedef void        (* DBusWatchToggledFunction)   (DBusWatch      *watch,
                                                    void           *data);
typedef void        (* DBusRemoveWatchFunction)    (DBusWatch      *watch,
                                                    void           *data);
typedef dbus_bool_t (* DBusAddTimeoutFunction)     (DBusTimeout    *timeout,
                                                    void           *data);
typedef void        (* DBusTimeoutToggledFunction) (DBusTimeout    *timeout,
                                                    void           *data);
typedef void        (* DBusRemoveTimeoutFunction)  (DBusTimeout    *timeout,
                                                    void           *data);
typedef void        (* DBusDispatchStatusFunction) (DBusConnection *connection,
                                                    DBusDispatchStatus new_status,
                                                    void           *data);
typedef void        (* DBusWakeupMainFunction)     (void           *data);
typedef dbus_bool_t (* DBusAllowUnixUserFunction)  (DBusConnection *connection,
                                                    unsigned long   uid,
                                                    void           *data);

typedef void (* DBusPendingCallNotifyFunction) (DBusPendingCall *pending,
                                                void            *user_data);


typedef DBusHandlerResult (* DBusHandleMessageFunction) (DBusConnection     *connection,
                                                         DBusMessage        *message,
                                                         void               *user_data);

pDBusMessage dbus_connection_send_with_reply_and_block(DBusConnection *connection,
							 DBusMessage *message,
							 int timeout_milliseconds,
							 DBusError *error);
pDBusConnection dbus_connection_open(const char *address, DBusError *error);
void dbus_connection_ref(DBusConnection *connection);
void dbus_connection_unref(/*@killref@*/ pDBusConnection connection);

dbus_bool_t dbus_connection_set_data(pDBusConnection connection,
				     dbus_int32_t slot,
				     void *data,
				     DBusFreeFunction free_data_func);

dbus_bool_t dbus_connection_allocate_data_slot(dbus_int32_t *slot_p);
/*@refcounted@*/ void* dbus_connection_get_data(pDBusConnection connection, dbus_int32_t slot);


#include <dbus/dbus-address.h>
#include <dbus/dbus-bus.h>
#include <dbus/dbus-connection.h>
#include <dbus/dbus-message.h>
#include <dbus/dbus-pending-call.h>
#include <dbus/dbus-protocol.h>
#include <dbus/dbus-server.h>
#include <dbus/dbus-threads.h>

#undef DBUS_INSIDE_DBUS_H

#endif /* MY_DBUS_H */

#else
/* If we're not using splint, just import the headers normally... */
# define DBUS_API_SUBJECT_TO_CHANGE
# include <dbus/dbus.h>
#endif
