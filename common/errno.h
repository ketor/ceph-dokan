#ifndef CEPH_ERRNO_H
#define CEPH_ERRNO_H

#include <string>
#define EALREADY 114 /* Operation already in progress */ //by ketor for CrushWrapper.cc
/* Return a given error code as a string */
std::string cpp_strerror(int err);

#endif
