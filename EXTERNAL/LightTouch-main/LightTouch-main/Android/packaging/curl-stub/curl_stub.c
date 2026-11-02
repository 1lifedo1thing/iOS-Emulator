/* LightTouch Android libcurl stand-in; see curl/curl.h. No network access:
 * every TSS request fails at curl_easy_init. */
#include <stdlib.h>
#include <string.h>
#include "curl/curl.h"

CURL *curl_easy_init(void) { return NULL; }
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...) { (void)handle; (void)option; return CURLE_FAILED_INIT; }
CURLcode curl_easy_perform(CURL *handle) { (void)handle; return CURLE_FAILED_INIT; }
void curl_easy_cleanup(CURL *handle) { (void)handle; }

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
	struct curl_slist *item = calloc(1, sizeof(*item));
	if (!item || !(item->data = strdup(data ? data : ""))) {
		free(item);
		return NULL;
	}
	if (!list)
		return item;
	struct curl_slist *last = list;
	while (last->next)
		last = last->next;
	last->next = item;
	return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
	while (list) {
		struct curl_slist *next = list->next;
		free(list->data);
		free(list);
		list = next;
	}
}
