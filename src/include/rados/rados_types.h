#ifndef CEPH_RADOS_TYPES_H
#define CEPH_RADOS_TYPES_H

#include "include/inttypes.h"

/**
 * @struct obj_watch_t
 * One item from list_watchers
 */
struct obj_watch_t {
  int64_t watcher_id;
  uint64_t cookie;
  uint32_t timeout_seconds;
}; 

typedef uint64_t snap_t;
#endif
