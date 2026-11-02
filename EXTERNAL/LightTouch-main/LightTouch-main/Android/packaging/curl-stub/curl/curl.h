/* LightTouch Android: configure/compile-time stand-in for libcurl.
 *
 * libtatsu (TSS signing requests) is a hard configure dependency of
 * libimobiledevice 1.4.0, but only its tools (ideviceimagemounter, restore
 * flows) call TSS. Neither packaged executable (usbmuxd-qemu, lt_appservice)
 * references a libtatsu symbol, so no libtatsu or curl object is linked into
 * them (verified by the build script's symbol check). This header declares
 * exactly the curl surface libtatsu 1.0.5 uses; curl_stub.c makes every
 * request fail (curl_easy_init returns NULL, which tss.c handles). */
#ifndef LIGHTTOUCH_CURL_STUB_H
#define LIGHTTOUCH_CURL_STUB_H
#include <stddef.h>
/* Like the real header (curl/system.h), which libtatsu relies on for time(). */
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURL;
typedef int CURLcode;
#define CURLE_OK 0
#define CURLE_FAILED_INIT 2
#define CURL_ERROR_SIZE 256

struct curl_slist {
	char *data;
	struct curl_slist *next;
};

typedef size_t (*curl_write_callback)(char *buffer, size_t size, size_t nitems, void *outstream);

typedef enum {
	CURLOPT_WRITEDATA = 10001,
	CURLOPT_URL = 10002,
	CURLOPT_ERRORBUFFER = 10010,
	CURLOPT_POSTFIELDS = 10015,
	CURLOPT_USERAGENT = 10018,
	CURLOPT_HTTPHEADER = 10023,
	CURLOPT_VERBOSE = 41,
	CURLOPT_POSTFIELDSIZE = 60,
	CURLOPT_SSL_VERIFYPEER = 64,
	CURLOPT_WRITEFUNCTION = 20011
} CURLoption;

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *handle);
void curl_easy_cleanup(CURL *handle);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data);
void curl_slist_free_all(struct curl_slist *list);

#ifdef __cplusplus
}
#endif
#endif
