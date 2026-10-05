#pragma once
#include <cstddef>
// Subset of libcurl's stable public C ABI; system curl headers take precedence.
extern "C" {
typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
typedef long long curl_off_t;
struct curl_slist { char* data; struct curl_slist* next; };
CURLcode curl_global_init(long);
void curl_global_cleanup(void);
CURL* curl_easy_init(void);
void curl_easy_cleanup(CURL*);
CURLcode curl_easy_setopt(CURL*,CURLoption,...);
CURLcode curl_easy_perform(CURL*);
CURLcode curl_easy_getinfo(CURL*,CURLINFO,...);
const char* curl_easy_strerror(CURLcode);
curl_slist* curl_slist_append(curl_slist*,const char*);
void curl_slist_free_all(curl_slist*);
}
#define CURL_GLOBAL_DEFAULT 3L
#define CURLE_OK 0
#define CURLE_OPERATION_TIMEDOUT 28
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_ERRORBUFFER 10010
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_NOPROGRESS 43
#define CURLOPT_XFERINFOFUNCTION 20219
#define CURLOPT_XFERINFODATA 10057
#define CURLOPT_TIMEOUT 13
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_POST 47
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_SSL_VERIFYHOST 81
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_PROTOCOLS_STR 10318
#define CURLOPT_LOW_SPEED_LIMIT 19
#define CURLOPT_LOW_SPEED_TIME 20
#define CURLINFO_RESPONSE_CODE 0x200002
