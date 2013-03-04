#ifndef CEPH_RADOS_TYPES_HPP
#define CEPH_RADOS_TYPES_HPP

#include <utility>
#include <vector>
#include "include/inttypes.h"
#include "include/rados/rados_types.h"

namespace librados {

struct clone_info_t {
  static const snap_t HEAD = ((snap_t)-1);
  snap_t cloneid;
  std::vector<snap_t> snaps;          // ascending
  std::vector< std::pair<uint64_t,uint64_t> > overlap;
  uint64_t size;
};

struct snap_set_t {
  std::vector<clone_info_t> clones;   // ascending
};

}
#endif
