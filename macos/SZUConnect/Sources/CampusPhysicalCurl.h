#ifndef SZU_CAMPUS_PHYSICAL_CURL_H
#define SZU_CAMPUS_PHYSICAL_CURL_H
#include <stddef.h>
#include <stdint.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SZUCampusCancellation SZUCampusCancellation;
typedef struct {
    const char *url;
    const char *resolved_addresses; /* comma-separated, verified IPv4 answers */
    const char *cookie;
    const char *source_ipv4;
    const char *interface_name;
    int (*route_is_current)(void *);
    void *route_context;
    uint32_t interface_index;
    double timeout_seconds;
    size_t response_limit;
} SZUCampusRequest;
typedef struct {
    unsigned char *body;
    size_t body_length;
    char *headers; /* final HTTP header block, including repeated Set-Cookie */
    size_t headers_length;
    long status;
    int error;
} SZUCampusResponse;
enum {
    SZU_CAMPUS_OK = 0, SZU_CAMPUS_CANCELLED = 1, SZU_CAMPUS_TIMEOUT = 2,
    SZU_CAMPUS_DNS = 3, SZU_CAMPUS_TLS = 4, SZU_CAMPUS_NETWORK = 5,
    SZU_CAMPUS_RESPONSE = 6, SZU_CAMPUS_TOO_LARGE = 7
};
SZUCampusCancellation *szu_campus_cancellation_create(void);
void szu_campus_cancellation_cancel(SZUCampusCancellation *token);
int szu_campus_cancellation_is_cancelled(const SZUCampusCancellation *token);
void szu_campus_cancellation_release(SZUCampusCancellation *token);
SZUCampusResponse szu_campus_request(const SZUCampusRequest *request, SZUCampusCancellation *token);
void szu_campus_response_release(SZUCampusResponse *response);
/* Validation uses the same gate as the actual native transfer, without opening sockets. */
int szu_campus_request_is_valid(const SZUCampusRequest *request);
#ifdef SZU_CAMPUS_CURL_TESTING
int szu_campus_test_bounds(void);
int szu_campus_test_bound_socket(uint32_t interface_index);
int szu_campus_test_broken_connection(uint32_t interface_index);
int szu_campus_test_security_options(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
#endif
