#include "foxterm.h"

#define USER_AGENT "FoxTerm/1.0 (TUI Browser; Firefox-backend)"

/* ── curl write callback ── */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FetchResult *r = (FetchResult *)userdata;
    size_t bytes = size * nmemb;
    char *tmp = realloc(r->data, r->size + bytes + 1);
    if (!tmp) return 0;
    r->data = tmp;
    memcpy(r->data + r->size, ptr, bytes);
    r->size += bytes;
    r->data[r->size] = '\0';
    return bytes;
}

/* ── Fetch a URL, return allocated FetchResult or NULL ── */
FetchResult *fetch_url(const char *url, const char *ua)
{
    FetchResult *r = calloc(1, sizeof(FetchResult));
    if (!r) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(r); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua ? ua : USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  /* auto decompress */

    /* Accept headers mimicking Firefox */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: ja,en-US;q=0.7,en;q=0.3");
    hdrs = curl_slist_append(hdrs, "DNT: 1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(r->data);
        free(r);
        return NULL;
    }

    return r;
}

void fetch_result_free(FetchResult *r)
{
    if (!r) return;
    free(r->data);
    free(r);
}