/*
 * bluetoold: a stand-in for BlueTool's XPC service (com.apple.BlueTool), for a controller that btd has already
 * booted. bluetoothd asks it to run a script ("boot", "init", ...) or a command before it opens the HCI transport;
 * the real BlueTool only knows Apple/Broadcom chips (no firmware entry for our ProductHash, then it waits for an
 * AppleOLYHAL Wi-Fi vendor and times out). This one answers every request with result 0 and an empty output, and
 * logs what was asked (/private/var/tmp/bluetoold.log).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

/* in libxpc on iOS too; the SDK marks it unavailable */
extern xpc_connection_t sweet_xpc_listener(const char *name, dispatch_queue_t q, uint64_t flags)
	__asm__("_xpc_connection_create_mach_service");

static FILE *logf;

static void request(xpc_connection_t peer, xpc_object_t msg)
{
	const char *script = xpc_dictionary_get_string(msg, "script");
	const char *command = xpc_dictionary_get_string(msg, "command");
	char *desc = xpc_copy_description(msg);
	fprintf(logf, "%ld: script %s command %s (%s)\n", (long)time(NULL), script ? script : "-", command ? command : "-",
		desc);
	free(desc);
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (reply) {
		xpc_dictionary_set_int64(reply, "result", 0);
		xpc_dictionary_set_string(reply, "output", "");
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
	}
}

int main(void)
{
	logf = fopen("/private/var/tmp/bluetoold.log", "a");
	if (!logf)
		logf = stderr;
	setvbuf(logf, NULL, _IOLBF, 0);
	xpc_connection_t l = sweet_xpc_listener("com.apple.BlueTool", NULL,
								XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
			return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
			if (xpc_get_type(msg) == XPC_TYPE_DICTIONARY)
				request(peer, msg);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	fprintf(logf, "%ld: bluetoold up\n", (long)time(NULL));
	dispatch_main();
}
