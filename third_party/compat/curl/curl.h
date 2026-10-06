#pragma once
#include <cstddef>
#include <sys/socket.h>
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
void curl_free(void*);
typedef void CURLU;
CURLU* curl_url(void);
void curl_url_cleanup(CURLU*);
int curl_url_set(CURLU*,int,const char*,unsigned);
int curl_url_get(const CURLU*,int,char**,unsigned);
typedef void CURLM;
struct CURLMsg { int msg; CURL* easy_handle; union { void* whatever; CURLcode result; } data; };
CURLM* curl_multi_init(void);
int curl_multi_cleanup(CURLM*);
int curl_multi_add_handle(CURLM*,CURL*);
int curl_multi_remove_handle(CURLM*,CURL*);
int curl_multi_perform(CURLM*,int*);
int curl_multi_poll(CURLM*,void*,unsigned,int,int*);
CURLMsg* curl_multi_info_read(CURLM*,int*);
struct curl_version_info_data { int age; const char* version; unsigned version_num; const char* host; int features; };
curl_version_info_data* curl_version_info(int);
struct curl_sockaddr { int family, socktype, protocol; unsigned addrlen; struct sockaddr addr; };
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
#define CURLOPT_HTTP_VERSION 84
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
#define CURLUPART_URL 0
#define CURLUPART_SCHEME 1
#define CURLUPART_USER 2
#define CURLUPART_PASSWORD 3
#define CURLUPART_HOST 5
#define CURLUPART_PORT 6
#define CURLUPART_PATH 7
#define CURLUPART_QUERY 8
#define CURLUPART_FRAGMENT 9
#define CURLU_URLDECODE (1U << 6)
#define CURLVERSION_FIRST 0
#define CURL_VERSION_ASYNCHDNS (1 << 7)
#define CURLMSG_DONE 1
#define CURL_SOCKET_BAD (-1)
#define CURLOPT_PROXY 10004
#define CURLOPT_USERAGENT 10018
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_HEADERFUNCTION 20079
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLOPT_OPENSOCKETFUNCTION 20163
#define CURLOPT_OPENSOCKETDATA 10164

// CURLINFO_DOUBLE + the offsets defined by curl's public curl.h.
#define CURLINFO_TOTAL_TIME 0x300003
#define CURLINFO_NAMELOOKUP_TIME 0x300004
#define CURLINFO_CONNECT_TIME 0x300005
#define CURLINFO_STARTTRANSFER_TIME 0x300011
#define CURLINFO_APPCONNECT_TIME 0x300021
