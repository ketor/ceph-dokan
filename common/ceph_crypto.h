#ifndef CEPH_CRYPTO_H
#define CEPH_CRYPTO_H

#include "acconfig.h"

#define CEPH_CRYPTO_MD5_DIGESTSIZE 16
#define CEPH_CRYPTO_HMACSHA1_DIGESTSIZE 20
#define CEPH_CRYPTO_SHA1_DIGESTSIZE 20
#define CEPH_CRYPTO_SHA256_DIGESTSIZE 32

#ifdef USE_CRYPTOPP
# define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <string.h>

namespace ceph {
  namespace crypto {
    void init(CephContext *cct);
    void shutdown();

    //by ketor using CryptoPP::Weak::MD5;
    //using CryptoPP::SHA1;
    //using CryptoPP::SHA256;
    //
    //class HMACSHA1: public CryptoPP::HMAC<CryptoPP::SHA1> {
    //public:
    //  HMACSHA1 (const byte *key, size_t length)
	//: CryptoPP::HMAC<CryptoPP::SHA1>(key, length)
	//{
	//}
    //  ~HMACSHA1();
    //};
  }
}

#else
# error "No supported crypto implementation found."
#endif

#endif
