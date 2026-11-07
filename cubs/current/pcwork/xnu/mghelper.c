/*
 * mghelper: stand-in for /usr/libexec/MobileGestaltHelper (the launchd plist of com.apple.mobilegestalt.xpc points Program at it).
 * The real helper never answers during iOS first boot on this port and lsd waits ~16 minutes for it. This one listens on the
 * same mach service, prints every request (xpc description) to stderr (= /dev/console through StandardErrorPath), and replies
 * at once with an empty reply dictionary, so callers get an answer ("no value") instead of waiting.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_error, _xpc_type_dictionary, _xpc_type_connection;
#define XPC_TYPE_ERROR (&_xpc_type_error)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_type_t xpc_get_type(xpc_object_t);
extern char *xpc_copy_description(xpc_object_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern void xpc_release(xpc_object_t);
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_resume(xpc_connection_t);

static int seen;

static void
handle(xpc_connection_t peer, xpc_object_t msg)
{
	if (xpc_get_type(msg) == XPC_TYPE_ERROR) {
		return;
	}
	if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) {
		return;
	}
	if (seen++ < 40) {
		char *d = xpc_copy_description(msg);

		fprintf(stderr, "mghelper: request #%d: %.600s\n", seen, d ? d : "?");
		free(d);
	}
	xpc_object_t reply = xpc_dictionary_create_reply(msg);

	if (reply) {
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	}
}

int
main(void)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	fprintf(stderr, "mghelper: started pid %d\n", getpid());
	xpc_connection_t svc = xpc_connection_create_mach_service("com.apple.mobilegestalt.xpc", NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);

	xpc_connection_set_event_handler(svc, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		fprintf(stderr, "mghelper: new connection\n");
		xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
			handle(peer, msg);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(svc);
	dispatch_main();
}
