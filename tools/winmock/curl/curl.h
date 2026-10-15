// Declaration-only subset for tools/check_windows_syntax.sh. This is neither
// libcurl nor a Windows ABI implementation, and must never be used at runtime.
// Native CMake targets use the pinned official libcurl headers and Schannel.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <winsock2.h>

using CURL = void;
using curl_off_t = std::int64_t;
using curl_socket_t = SOCKET;
constexpr curl_socket_t CURL_SOCKET_BAD = INVALID_SOCKET;
enum curlsocktype { CURLSOCKTYPE_IPCXN, CURLSOCKTYPE_ACCEPT };
struct curl_sockaddr {
    int family;
    int socktype;
    int protocol;
    unsigned int addrlen;
    SOCKADDR addr;
};
struct curl_slist { char* data; curl_slist* next; };

// Tags only: mock builds do not call these functions or validate option numbers.
enum CURLcode {
    CURLE_OK, CURLE_UNSUPPORTED_PROTOCOL, CURLE_FAILED_INIT, CURLE_URL_MALFORMAT,
    CURLE_COULDNT_RESOLVE_HOST, CURLE_WRITE_ERROR, CURLE_OPERATION_TIMEDOUT,
    CURLE_SSL_CONNECT_ERROR, CURLE_ABORTED_BY_CALLBACK, CURLE_SSL_CERTPROBLEM,
    CURLE_SSL_CIPHER, CURLE_PEER_FAILED_VERIFICATION, CURLE_FILESIZE_EXCEEDED,
    CURLE_SSL_CACERT_BADFILE, CURLE_SSL_ISSUER_ERROR, CURLE_SSL_PINNEDPUBKEYNOTMATCH,
    CURLE_SSL_CLIENTCERT, CURLE_SSL_ENGINE_NOTFOUND, CURLE_SSL_ENGINE_SETFAILED,
    CURLE_SSL_ENGINE_INITFAILED, CURLE_SSL_CRL_BADFILE, CURLE_SSL_INVALIDCERTSTATUS,
    CURLE_SSL_SHUTDOWN_FAILED, CURLE_USE_SSL_FAILED
};
enum CURLoption {
    CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST, CURLOPT_SSLVERSION,
    CURLOPT_SSL_OPTIONS, CURLOPT_SSLCERT, CURLOPT_SSLKEY, CURLOPT_FOLLOWLOCATION,
    CURLOPT_MAXREDIRS, CURLOPT_UNRESTRICTED_AUTH, CURLOPT_HTTPAUTH, CURLOPT_PROXYAUTH,
    CURLOPT_NETRC, CURLOPT_PROXY, CURLOPT_PRE_PROXY, CURLOPT_NOPROXY, CURLOPT_NOSIGNAL,
    CURLOPT_IPRESOLVE, CURLOPT_PROTOCOLS_STR, CURLOPT_REDIR_PROTOCOLS_STR,
    CURLOPT_DNS_CACHE_TIMEOUT, CURLOPT_FRESH_CONNECT, CURLOPT_FORBID_REUSE,
    CURLOPT_VERBOSE, CURLOPT_URL, CURLOPT_HTTPGET, CURLOPT_TIMEOUT_MS,
    CURLOPT_CONNECTTIMEOUT_MS, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA,
    CURLOPT_HEADERFUNCTION, CURLOPT_HEADERDATA, CURLOPT_OPENSOCKETFUNCTION,
    CURLOPT_OPENSOCKETDATA, CURLOPT_XFERINFOFUNCTION, CURLOPT_XFERINFODATA,
    CURLOPT_NOPROGRESS, CURLOPT_PREREQFUNCTION, CURLOPT_PREREQDATA,
    CURLOPT_MAXFILESIZE_LARGE, CURLOPT_HTTPHEADER, CURLOPT_RESOLVE, CURLOPT_COOKIE
};
enum CURLINFO { CURLINFO_RESPONSE_CODE };
enum CURLversion { CURLVERSION_TWELFTH };
constexpr CURLversion CURLVERSION_NOW = CURLVERSION_TWELFTH;
// Only fields read by this source are represented; this is intentionally not an ABI layout.
struct curl_version_info_data { const char* ssl_version; };
constexpr int CURL_PREREQFUNC_OK = 0, CURL_PREREQFUNC_ABORT = 1;
constexpr long CURL_SSLVERSION_TLSv1_2 = 6;
constexpr long CURLSSLOPT_NATIVE_CA = 1L << 4;
constexpr unsigned long CURLAUTH_NONE = 0;
constexpr long CURL_NETRC_IGNORED = 0, CURL_IPRESOLVE_V4 = 1;
constexpr long CURL_GLOBAL_DEFAULT = 3;

extern "C" {
CURLcode curl_global_init(long flags);
CURL* curl_easy_init();
void curl_easy_cleanup(CURL* handle);
CURLcode curl_easy_setopt(CURL* handle, CURLoption option, ...);
CURLcode curl_easy_getinfo(CURL* handle, CURLINFO info, ...);
CURLcode curl_easy_perform(CURL* handle);
curl_slist* curl_slist_append(curl_slist* list, const char* text);
void curl_slist_free_all(curl_slist* list);
std::time_t curl_getdate(const char* date, const std::time_t* unused);
curl_version_info_data* curl_version_info(CURLversion version);
}
