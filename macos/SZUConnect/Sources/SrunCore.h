// SRun wire-format core adapted from ATMLuck/AutoSZUWeb src/srun.cpp.
// Distributed under the MIT license; see Resources/AutoSZUWeb-LICENSE.txt.
#ifndef SZU_SRUN_CORE_H
#define SZU_SRUN_CORE_H
#include <stddef.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include "CampusPhysicalCurl.h"
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Caller owns returned UTF-8 string and must release it with szu_release.
char *szu_xencode_base64(const unsigned char *bytes, size_t length,
                        const unsigned char *key, size_t key_length);
char *szu_hmac_md5(const unsigned char *bytes, size_t length,
                    const unsigned char *key, size_t key_length);
char *szu_sha1(const unsigned char *bytes, size_t length);
void szu_release(char *value);
#ifdef __cplusplus
}
#endif
#endif
