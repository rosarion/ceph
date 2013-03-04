#include <errno.h>

#include <string>
#include <map>

#include "common/errno.h"
#include "rgw_rados.h"
#include "rgw_acl.h"

#include "include/types.h"
#include "rgw_user.h"
#include "rgw_string.h"

// until everything is moved from rgw_common
#include "rgw_common.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;


/**
 * Get the anonymous (ie, unauthenticated) user info.
 */
void rgw_get_anon_user(RGWUserInfo& info)
{
  info.user_id = RGW_USER_ANON_ID;
  info.display_name.clear();
  info.access_keys.clear();
}

bool rgw_user_is_authenticated(RGWUserInfo& info)
{
  return (info.user_id != RGW_USER_ANON_ID);
}

/**
 * Save the given user information to storage.
 * Returns: 0 on success, -ERR# on failure.
 */
int rgw_store_user_info(RGWRados *store, RGWUserInfo& info, RGWUserInfo *old_info, bool exclusive)
{
  bufferlist bl;
  info.encode(bl);
  string md5;
  int ret;
  map<string,bufferlist> attrs;

  map<string, RGWAccessKey>::iterator iter;
  for (iter = info.swift_keys.begin(); iter != info.swift_keys.end(); ++iter) {
    if (old_info && old_info->swift_keys.count(iter->first) != 0)
      continue;
    RGWAccessKey& k = iter->second;
    /* check if swift mapping exists */
    RGWUserInfo inf;
    int r = rgw_get_user_info_by_swift(store, k.id, inf);
    if (r >= 0 && inf.user_id.compare(info.user_id) != 0) {
      ldout(store->ctx(), 0) << "WARNING: can't store user info, swift id already mapped to another user" << dendl;
      return -EEXIST;
    }
  }

  if (!info.access_keys.empty()) {
    /* check if access keys already exist */
    RGWUserInfo inf;
    map<string, RGWAccessKey>::iterator iter = info.access_keys.begin();
    for (; iter != info.access_keys.end(); ++iter) {
      RGWAccessKey& k = iter->second;
      if (old_info && old_info->access_keys.count(iter->first) != 0)
        continue;
      int r = rgw_get_user_info_by_access_key(store, k.id, inf);
      if (r >= 0 && inf.user_id.compare(info.user_id) != 0) {
        ldout(store->ctx(), 0) << "WARNING: can't store user info, access key already mapped to another user" << dendl;
        return -EEXIST;
      }
    }
  }

  RGWUID ui;
  ui.user_id = info.user_id;

  bufferlist link_bl;
  ::encode(ui, link_bl);

  bufferlist data_bl;
  ::encode(ui, data_bl);
  ::encode(info, data_bl);

  ret = rgw_put_system_obj(store, store->params.user_uid_pool, info.user_id, data_bl.c_str(), data_bl.length(), exclusive);
  if (ret < 0)
    return ret;

  if (!info.user_email.empty()) {
    if (!old_info ||
        old_info->user_email.compare(info.user_email) != 0) { /* only if new index changed */
      ret = rgw_put_system_obj(store, store->params.user_email_pool, info.user_email, link_bl.c_str(), link_bl.length(), exclusive);
      if (ret < 0)
        return ret;
    }
  }

  if (!info.access_keys.empty()) {
    map<string, RGWAccessKey>::iterator iter = info.access_keys.begin();
    for (; iter != info.access_keys.end(); ++iter) {
      RGWAccessKey& k = iter->second;
      if (old_info && old_info->access_keys.count(iter->first) != 0)
	continue;

      ret = rgw_put_system_obj(store, store->params.user_keys_pool, k.id, link_bl.c_str(), link_bl.length(), exclusive);
      if (ret < 0)
        return ret;
    }
  }

  map<string, RGWAccessKey>::iterator siter;
  for (siter = info.swift_keys.begin(); siter != info.swift_keys.end(); ++siter) {
    RGWAccessKey& k = siter->second;
    if (old_info && old_info->swift_keys.count(siter->first) != 0)
      continue;

    ret = rgw_put_system_obj(store, store->params.user_swift_pool, k.id, link_bl.c_str(), link_bl.length(), exclusive);
    if (ret < 0)
      return ret;
  }

  return ret;
}

int rgw_get_user_info_from_index(RGWRados *store, string& key, rgw_bucket& bucket, RGWUserInfo& info)
{
  bufferlist bl;
  RGWUID uid;

  int ret = rgw_get_obj(store, NULL, bucket, key, bl);
  if (ret < 0)
    return ret;

  bufferlist::iterator iter = bl.begin();
  try {
    ::decode(uid, iter);
    return rgw_get_user_info_by_uid(store, uid.user_id, info);
  } catch (buffer::error& err) {
    ldout(store->ctx(), 0) << "ERROR: failed to decode user info, caught buffer::error" << dendl;
    return -EIO;
  }

  return 0;
}

/**
 * Given an email, finds the user info associated with it.
 * returns: 0 on success, -ERR# on failure (including nonexistence)
 */
int rgw_get_user_info_by_uid(RGWRados *store, string& uid, RGWUserInfo& info)
{
  bufferlist bl;
  RGWUID user_id;

  int ret = rgw_get_obj(store, NULL, store->params.user_uid_pool, uid, bl);
  if (ret < 0)
    return ret;

  bufferlist::iterator iter = bl.begin();
  try {
    ::decode(user_id, iter);
    if (user_id.user_id.compare(uid) != 0) {
      lderr(store->ctx())  << "ERROR: rgw_get_user_info_by_uid(): user id mismatch: " << user_id.user_id << " != " << uid << dendl;
      return -EIO;
    }
    if (!iter.end()) {
      ::decode(info, iter);
    }
  } catch (buffer::error& err) {
    ldout(store->ctx(), 0) << "ERROR: failed to decode user info, caught buffer::error" << dendl;
    return -EIO;
  }

  return 0;
}

/**
 * Given an email, finds the user info associated with it.
 * returns: 0 on success, -ERR# on failure (including nonexistence)
 */
int rgw_get_user_info_by_email(RGWRados *store, string& email, RGWUserInfo& info)
{
  return rgw_get_user_info_from_index(store, email, store->params.user_email_pool, info);
}

/**
 * Given an swift username, finds the user_info associated with it.
 * returns: 0 on success, -ERR# on failure (including nonexistence)
 */
extern int rgw_get_user_info_by_swift(RGWRados *store, string& swift_name, RGWUserInfo& info)
{
  return rgw_get_user_info_from_index(store, swift_name, store->params.user_swift_pool, info);
}

/**
 * Given an access key, finds the user info associated with it.
 * returns: 0 on success, -ERR# on failure (including nonexistence)
 */
extern int rgw_get_user_info_by_access_key(RGWRados *store, string& access_key, RGWUserInfo& info)
{
  return rgw_get_user_info_from_index(store, access_key, store->params.user_keys_pool, info);
}

static void get_buckets_obj(string& user_id, string& buckets_obj_id)
{
  buckets_obj_id = user_id;
  buckets_obj_id += RGW_BUCKETS_OBJ_PREFIX;
}

static int rgw_read_buckets_from_attr(RGWRados *store, string& user_id, RGWUserBuckets& buckets)
{
  bufferlist bl;
  rgw_obj obj(store->params.user_uid_pool, user_id);
  int ret = store->get_attr(NULL, obj, RGW_ATTR_BUCKETS, bl);
  if (ret)
    return ret;

  bufferlist::iterator iter = bl.begin();
  try {
    buckets.decode(iter);
  } catch (buffer::error& err) {
    ldout(store->ctx(), 0) << "ERROR: failed to decode buckets info, caught buffer::error" << dendl;
    return -EIO;
  }
  return 0;
}

/**
 * Get all the buckets owned by a user and fill up an RGWUserBuckets with them.
 * Returns: 0 on success, -ERR# on failure.
 */
int rgw_read_user_buckets(RGWRados *store, string user_id, RGWUserBuckets& buckets, bool need_stats)
{
  int ret;
  buckets.clear();
  if (store->supports_omap()) {
    string buckets_obj_id;
    get_buckets_obj(user_id, buckets_obj_id);
    bufferlist bl;
    rgw_obj obj(store->params.user_uid_pool, buckets_obj_id);
    bufferlist header;
    map<string,bufferlist> m;

    ret = store->omap_get_all(obj, header, m);
    if (ret == -ENOENT)
      ret = 0;

    if (ret < 0)
      return ret;

    for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); q++) {
      bufferlist::iterator iter = q->second.begin();
      RGWBucketEnt bucket;
      ::decode(bucket, iter);
      buckets.add(bucket);
    }
  } else {
    ret = rgw_read_buckets_from_attr(store, user_id, buckets);
    switch (ret) {
    case 0:
      break;
    case -ENODATA:
      ret = 0;
      return 0;
    default:
      return ret;
    }
  }

  list<string> buckets_list;

  if (need_stats) {
    map<string, RGWBucketEnt>& m = buckets.get_buckets();
    int r = store->update_containers_stats(m);
    if (r < 0)
      ldout(store->ctx(), 0) << "ERROR: could not get stats for buckets" << dendl;

  }
  return 0;
}

/**
 * Store the set of buckets associated with a user on a n xattr
 * not used with all backends
 * This completely overwrites any previously-stored list, so be careful!
 * Returns 0 on success, -ERR# otherwise.
 */
int rgw_write_buckets_attr(RGWRados *store, string user_id, RGWUserBuckets& buckets)
{
  bufferlist bl;
  buckets.encode(bl);

  rgw_obj obj(store->params.user_uid_pool, user_id);

  int ret = store->set_attr(NULL, obj, RGW_ATTR_BUCKETS, bl);

  return ret;
}

int rgw_add_bucket(RGWRados *store, string user_id, rgw_bucket& bucket)
{
  int ret;
  string& bucket_name = bucket.name;

  if (store->supports_omap()) {
    bufferlist bl;

    RGWBucketEnt new_bucket;
    new_bucket.bucket = bucket;
    new_bucket.size = 0;
    time(&new_bucket.mtime);
    ::encode(new_bucket, bl);

    string buckets_obj_id;
    get_buckets_obj(user_id, buckets_obj_id);

    rgw_obj obj(store->params.user_uid_pool, buckets_obj_id);
    ret = store->omap_set(obj, bucket_name, bl);
    if (ret < 0) {
      ldout(store->ctx(), 0) << "ERROR: error adding bucket to directory: "
          << cpp_strerror(-ret)<< dendl;
    }
  } else {
    RGWUserBuckets buckets;

    ret = rgw_read_user_buckets(store, user_id, buckets, false);
    RGWBucketEnt new_bucket;

    switch (ret) {
    case 0:
    case -ENOENT:
    case -ENODATA:
      new_bucket.bucket = bucket;
      new_bucket.size = 0;
      time(&new_bucket.mtime);
      buckets.add(new_bucket);
      ret = rgw_write_buckets_attr(store, user_id, buckets);
      break;
    default:
      ldout(store->ctx(), 10) << "rgw_write_buckets_attr returned " << ret << dendl;
      break;
    }
  }

  return ret;
}

int rgw_remove_user_bucket_info(RGWRados *store, string user_id, rgw_bucket& bucket)
{
  int ret;

  if (store->supports_omap()) {
    bufferlist bl;

    string buckets_obj_id;
    get_buckets_obj(user_id, buckets_obj_id);

    rgw_obj obj(store->params.user_uid_pool, buckets_obj_id);
    ret = store->omap_del(obj, bucket.name);
    if (ret < 0) {
      ldout(store->ctx(), 0) << "ERROR: error removing bucket from directory: "
          << cpp_strerror(-ret)<< dendl;
    }
  } else {
    RGWUserBuckets buckets;

    ret = rgw_read_user_buckets(store, user_id, buckets, false);

    if (ret == 0 || ret == -ENOENT) {
      buckets.remove(bucket.name);
      ret = rgw_write_buckets_attr(store, user_id, buckets);
    }
  }

  return ret;
}

int rgw_remove_key_index(RGWRados *store, RGWAccessKey& access_key)
{
  rgw_obj obj(store->params.user_keys_pool, access_key.id);
  int ret = store->delete_obj(NULL, obj);
  return ret;
}

int rgw_remove_uid_index(RGWRados *store, string& uid)
{
  rgw_obj obj(store->params.user_uid_pool, uid);
  int ret = store->delete_obj(NULL, obj);
  return ret;
}

int rgw_remove_email_index(RGWRados *store, string& email)
{
  rgw_obj obj(store->params.user_email_pool, email);
  int ret = store->delete_obj(NULL, obj);
  return ret;
}

int rgw_remove_swift_name_index(RGWRados *store, string& swift_name)
{
  rgw_obj obj(store->params.user_swift_pool, swift_name);
  int ret = store->delete_obj(NULL, obj);
  return ret;
}

/**
 * delete a user's presence from the RGW system.
 * First remove their bucket ACLs, then delete them
 * from the user and user email pools. This leaves the pools
 * themselves alone, as well as any ACLs embedded in object xattrs.
 */
int rgw_delete_user(RGWRados *store, RGWUserInfo& info) {
  RGWUserBuckets user_buckets;
  int ret = rgw_read_user_buckets(store, info.user_id, user_buckets, false);
  if (ret < 0)
    return ret;

  map<string, RGWBucketEnt>& buckets = user_buckets.get_buckets();
  vector<rgw_bucket> buckets_vec;
  for (map<string, RGWBucketEnt>::iterator i = buckets.begin();
      i != buckets.end();
      ++i) {
    RGWBucketEnt& bucket = i->second;
    buckets_vec.push_back(bucket.bucket);
  }
  map<string, RGWAccessKey>::iterator kiter = info.access_keys.begin();
  for (; kiter != info.access_keys.end(); ++kiter) {
    ldout(store->ctx(), 10) << "removing key index: " << kiter->first << dendl;
    ret = rgw_remove_key_index(store, kiter->second);
    if (ret < 0 && ret != -ENOENT) {
      ldout(store->ctx(), 0) << "ERROR: could not remove " << kiter->first << " (access key object), should be fixed (err=" << ret << ")" << dendl;
      return ret;
    }
  }

  map<string, RGWAccessKey>::iterator siter = info.swift_keys.begin();
  for (; siter != info.swift_keys.end(); ++siter) {
    RGWAccessKey& k = siter->second;
    ldout(store->ctx(), 10) << "removing swift subuser index: " << k.id << dendl;
    /* check if swift mapping exists */
    ret = rgw_remove_swift_name_index(store, k.id);
    if (ret < 0 && ret != -ENOENT) {
      ldout(store->ctx(), 0) << "ERROR: could not remove " << k.id << " (swift name object), should be fixed (err=" << ret << ")" << dendl;
      return ret;
    }
  }

  rgw_obj email_obj(store->params.user_email_pool, info.user_email);
  ldout(store->ctx(), 10) << "removing email index: " << info.user_email << dendl;
  ret = store->delete_obj(NULL, email_obj);
  if (ret < 0 && ret != -ENOENT) {
    ldout(store->ctx(), 0) << "ERROR: could not remove " << info.user_id << ":" << email_obj << ", should be fixed (err=" << ret << ")" << dendl;
    return ret;
  }

  string buckets_obj_id;
  get_buckets_obj(info.user_id, buckets_obj_id);
  rgw_obj uid_bucks(store->params.user_uid_pool, buckets_obj_id);
  ldout(store->ctx(), 10) << "removing user buckets index" << dendl;
  ret = store->delete_obj(NULL, uid_bucks);
  if (ret < 0 && ret != -ENOENT) {
    ldout(store->ctx(), 0) << "ERROR: could not remove " << info.user_id << ":" << uid_bucks << ", should be fixed (err=" << ret << ")" << dendl;
    return ret;
  }
  
  rgw_obj uid_obj(store->params.user_uid_pool, info.user_id);
  ldout(store->ctx(), 10) << "removing user index: " << info.user_id << dendl;
  ret = store->delete_obj(NULL, uid_obj);
  if (ret < 0 && ret != -ENOENT) {
    ldout(store->ctx(), 0) << "ERROR: could not remove " << info.user_id << ":" << uid_obj << ", should be fixed (err=" << ret << ")" << dendl;
    return ret;
  }

  return 0;
}

static bool char_is_unreserved_url(char c)
{
  if (isalnum(c))
    return true;

  switch (c) {
  case '-':
  case '.':
  case '_':
  case '~':
    return true;
  default:
    return false;
  }
}

// define as static when changes complete
bool validate_access_key(string& key)
{
  const char *p = key.c_str();
  while (*p) {
    if (!char_is_unreserved_url(*p))
      return false;
    p++;
  }
  return true;
}

// define as static when changes complete
int remove_object(RGWRados *store, rgw_bucket& bucket, std::string& object)
{
  int ret = -EINVAL;
  RGWRadosCtx rctx(store);

  rgw_obj obj(bucket,object);

  ret = store->delete_obj((void *)&rctx, obj);

  return ret;
}

// define as static when changes complete
int remove_bucket(RGWRados *store, rgw_bucket& bucket, bool delete_children)
{
  int ret;
  map<RGWObjCategory, RGWBucketStats> stats;
  std::vector<RGWObjEnt> objs;
  std::string prefix, delim, marker, ns;
  map<string, bool> common_prefixes;
  rgw_obj obj;
  RGWBucketInfo info;
  bufferlist bl;

  ret = store->get_bucket_stats(bucket, stats);
  if (ret < 0)
    return ret;

  obj.bucket = bucket;
  int max = 1000;

  ret = rgw_get_obj(store, NULL, store->params.domain_root,\
           bucket.name, bl, NULL);

  bufferlist::iterator iter = bl.begin();
  try {
    ::decode(info, iter);
  } catch (buffer::error& err) {
    //cerr << "ERROR: could not decode buffer info, caught buffer::error" << std::endl;
    return -EIO;
  }

  if (delete_children) {
    ret = store->list_objects(bucket, max, prefix, delim, marker,\
            objs, common_prefixes,\
            false, ns, (bool *)false, NULL);

    if (ret < 0)
      return ret;

    while (objs.size() > 0) {
      std::vector<RGWObjEnt>::iterator it = objs.begin();
      for (it = objs.begin(); it != objs.end(); it++) {
        ret = remove_object(store, bucket, (*it).name);
        if (ret < 0)
          return ret;
      }
      objs.clear();

      ret = store->list_objects(bucket, max, prefix, delim, marker, objs, common_prefixes,
                                false, ns, (bool *)false, NULL);
      if (ret < 0)
        return ret;
    }
  }

  ret = store->delete_bucket(bucket);
  if (ret < 0) {
    //cerr << "ERROR: could not remove bucket " << bucket.name << std::endl;

    return ret;
  }

  ret = rgw_remove_user_bucket_info(store, info.owner, bucket);
  if (ret < 0) {
    //cerr << "ERROR: unable to remove user bucket information" << std::endl;
  }

  return ret;
}

static void set_err_msg(std::string *sink, std::string msg)
{
  if (sink && !msg.empty())
    *sink = msg;
}

static bool remove_old_indexes(RGWRados *store,
         RGWUserInfo& old_info, RGWUserInfo& new_info, std::string *err_msg)
{
  int ret;
  bool success = true;

  if (!old_info.user_id.empty() && old_info.user_id.compare(new_info.user_id) != 0) {
    ret = rgw_remove_uid_index(store, old_info.user_id);
    if (ret < 0 && ret != -ENOENT) {
      set_err_msg(err_msg, "ERROR: could not remove index for uid " + old_info.user_id);
      success = false;
    }
  }

  if (!old_info.user_email.empty() &&
      old_info.user_email.compare(new_info.user_email) != 0) {
    ret = rgw_remove_email_index(store, old_info.user_email);
  if (ret < 0 && ret != -ENOENT) {
      set_err_msg(err_msg, "ERROR: could not remove index for email " + old_info.user_email);
      success = false;
    }
  }

  map<string, RGWAccessKey>::iterator old_iter;
  for (old_iter = old_info.swift_keys.begin(); old_iter != old_info.swift_keys.end(); ++old_iter) {
    RGWAccessKey& swift_key = old_iter->second;
    map<string, RGWAccessKey>::iterator new_iter = new_info.swift_keys.find(swift_key.id);
    if (new_iter == new_info.swift_keys.end()) {
      ret = rgw_remove_swift_name_index(store, swift_key.id);
      if (ret < 0 && ret != -ENOENT) {
        set_err_msg(err_msg, "ERROR: could not remove index for swift_name " + swift_key.id);
        success = false;
      }
    }
  }

  return success;
}



RGWAccessKeyPool::RGWAccessKeyPool(RGWUser* usr)
{
  user = usr;

  if (user == NULL || user->has_failed()) {
    keys_allowed = false;
    return;
  }

  keys_allowed = true;

  store = user->get_store();
}

RGWAccessKeyPool::~RGWAccessKeyPool()
{

}

int RGWAccessKeyPool::init(RGWUserAdminOperation& op)
{
  if (!op.is_initialized()) {
    keys_allowed = false;
    return -EINVAL;
  }

  std::string uid = op.get_user_id();
  if (uid.compare(RGW_USER_ANON_ID) == 0) {
    keys_allowed = false;
    return -EACCES;
  }

  swift_keys = op.get_swift_keys();
  access_keys = op.get_access_keys();

  keys_allowed = true;

  return 0;
}

bool RGWAccessKeyPool::check_existing_key(RGWUserAdminOperation& op)
{
  bool existing_key = false;

  int key_type = op.get_key_type();
  std::string kid = op.get_access_key();
  std::map<std::string, RGWAccessKey>::iterator kiter;
  std::string swift_kid = op.build_default_swift_kid();

  RGWUserInfo dup_info;

  if (kid.empty() && swift_kid.empty())
    return false;

  switch (key_type) {
  case KEY_TYPE_SWIFT:
    kiter = swift_keys->find(kid);

    existing_key = (kiter != swift_keys->end());
    if (existing_key)
      break;

    if (swift_kid.empty())
      return false;

    kiter = swift_keys->find(swift_kid);

    existing_key = (kiter != swift_keys->end());
    if (existing_key)
      op.set_access_key(swift_kid);

    break;
  case KEY_TYPE_S3:
    kiter = access_keys->find(kid);
    existing_key = (kiter != access_keys->end());

    break;
  default:
    kiter = access_keys->find(kid);

    existing_key = (kiter != access_keys->end());
    if (existing_key) {
      op.set_key_type(KEY_TYPE_S3);
      break;
    }

    kiter = swift_keys->find(kid);

    existing_key = (kiter != swift_keys->end());
    if (existing_key) {
      op.set_key_type(KEY_TYPE_SWIFT);
      break;
    }

    if (swift_kid.empty())
      return false;

    kiter = swift_keys->find(swift_kid);

    existing_key = (kiter != swift_keys->end());
    if (existing_key) {
      op.set_access_key(swift_kid);
      op.set_key_type(KEY_TYPE_SWIFT);
    }
  }

  if (existing_key)
    op.set_existing_key();

  return existing_key;
}

int RGWAccessKeyPool::check_op(RGWUserAdminOperation& op,
     std::string *err_msg)
{
  std::string subprocess_msg;
  RGWUserInfo dup_info;

  if (!op.is_populated()) {
    set_err_msg(err_msg, "user info was not populated");
    return -EINVAL;
  }

  if (!keys_allowed) {
    set_err_msg(err_msg, "keys not allowed for this user");
    return -EACCES;
  }

  std::string access_key = op.get_access_key();
  std::string secret_key = op.get_secret_key();

  /* see if the access key or secret key was specified */
  if (!op.will_gen_access() && access_key.empty()) {
    set_err_msg(err_msg, "empty access key");
    return -EINVAL;
  }

  if (!op.will_gen_secret() && secret_key.empty()) {
    set_err_msg(err_msg, "empty secret key");
    return -EINVAL;
  }

  check_existing_key(op);

  // if a key type wasn't specified set it to s3
  if (op.get_key_type() < 0)
    op.set_key_type(KEY_TYPE_S3);

  return 0;
}

// Generate a new random key
int RGWAccessKeyPool::generate_key(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string duplicate_check_id;
  std::string id;
  std::string key;

  std::pair<std::string, RGWAccessKey> key_pair;
  RGWAccessKey new_key;
  RGWUserInfo duplicate_check;

  int ret = 0;
  int key_type = op.get_key_type();
  bool gen_access = op.will_gen_access();
  bool gen_secret = op.will_gen_secret();
  std::string subuser = op.get_subuser();

  if (!keys_allowed) {
    set_err_msg(err_msg, "access keys not allowed for this user");
    return -EACCES;
  }

  if (op.has_existing_key()) {
    set_err_msg(err_msg, "cannot create existing key");
    return -EEXIST;
  }

  if (!gen_access)
    id = op.get_access_key();

  if (!id.empty())
    switch (key_type) {
    case KEY_TYPE_SWIFT:
      if (rgw_get_user_info_by_swift(store, id, duplicate_check) >= 0) {
        set_err_msg(err_msg, "existing swift key in RGW system:" + id);
        return -EEXIST;
      }
    case KEY_TYPE_S3:
      if (rgw_get_user_info_by_swift(store, id, duplicate_check) >= 0) {
        set_err_msg(err_msg, "existing s3 key in RGW system:" + id);
        return -EEXIST;
      }
    }

  if (op.has_subuser())
    new_key.subuser = op.get_subuser();

  if (!gen_secret) {
    key = op.get_secret_key();
  } else if (gen_secret) {
    char secret_key_buf[SECRET_KEY_LEN + 1];

    ret = gen_rand_base64(g_ceph_context, secret_key_buf, sizeof(secret_key_buf));
    if (ret < 0) {
      set_err_msg(err_msg, "unable to generate secret key");
      return ret;
    }

    key = secret_key_buf;
  }

  // Generate the access key
  if (key_type == KEY_TYPE_S3 && gen_access) {
    char public_id_buf[PUBLIC_ID_LEN + 1];

    do {
      int id_buf_size = sizeof(public_id_buf);
      ret = gen_rand_alphanumeric_upper(g_ceph_context,
               public_id_buf, id_buf_size);

      if (ret < 0) {
        set_err_msg(err_msg, "unable to generate access key");
        return ret;
      }

      id = public_id_buf;
      if (!validate_access_key(id))
        continue;

    } while (!rgw_get_user_info_by_access_key(store, id, duplicate_check));
  }

  if (key_type == KEY_TYPE_SWIFT && gen_access) {
    id = op.build_default_swift_kid();
    if (id.empty()) {
      set_err_msg(err_msg, "empty swift access key");
      return -EINVAL;
    }

    // check that the access key doesn't exist
    if (rgw_get_user_info_by_swift(store, id, duplicate_check) >= 0) {
      set_err_msg(err_msg, "cannot create existing swift key");
      return -EEXIST;
    }
  }

  // finally create the new key
  new_key.id = id;
  new_key.key = key;

  key_pair.first = id;
  key_pair.second = new_key;

  if (key_type == KEY_TYPE_S3)
    access_keys->insert(key_pair);
  else if (key_type == KEY_TYPE_SWIFT)
    swift_keys->insert(key_pair);

  return 0;
}

// modify an existing key
int RGWAccessKeyPool::modify_key(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string id = op.get_access_key();
  std::string key = op.get_secret_key();
  int key_type = op.get_key_type();

  RGWAccessKey modify_key;

  pair<string, RGWAccessKey> key_pair;
  map<std::string, RGWAccessKey>::iterator kiter;

  if (id.empty()) {
    set_err_msg(err_msg, "no access key specified");
    return -EINVAL;
  }

  if (!op.has_existing_key()) {
    set_err_msg(err_msg, "key does not exist");
    return -EINVAL;
  }

  key_pair.first = id;

  if (key_type == KEY_TYPE_SWIFT) {
    kiter = swift_keys->find(id);
    modify_key = kiter->second;
  } else if (key_type == KEY_TYPE_S3) {
    kiter = access_keys->find(id);
    modify_key = kiter->second;
  } else {
    set_err_msg(err_msg, "invalid key type");
    return -EINVAL;
  }

  if (op.will_gen_secret()) {
    char secret_key_buf[SECRET_KEY_LEN + 1];

    int ret;
    int key_buf_size = sizeof(secret_key_buf);
    ret  = gen_rand_base64(g_ceph_context, secret_key_buf, key_buf_size);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to generate secret key");
      return ret;
    }

    key = secret_key_buf;
  }

  if (key.empty()) {
      set_err_msg(err_msg, "empty secret key");
      return  -EINVAL;
  }

  // update the access key with the new secret key
  modify_key.key = key;
  key_pair.second = modify_key;


  if (key_type == KEY_TYPE_S3)
    access_keys->insert(key_pair);

  else if (key_type == KEY_TYPE_SWIFT)
    swift_keys->insert(key_pair);

  return 0;
}

int RGWAccessKeyPool::execute_add(RGWUserAdminOperation& op,
         std::string *err_msg, bool defer_user_update)
{
  int ret = 0;

  std::string subprocess_msg;
  int key_op = GENERATE_KEY;

  // set the op
  if (op.has_existing_key())
    key_op = MODIFY_KEY;

  switch (key_op) {
  case GENERATE_KEY:
    ret = generate_key(op, &subprocess_msg);
    break;
  case MODIFY_KEY:
    ret = modify_key(op, &subprocess_msg);
    break;
  }

  if (ret < 0)
    return ret;

  // store the updated info
  if (!defer_user_update)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWAccessKeyPool::add(RGWUserAdminOperation& op, std::string *err_msg)
{
  return add(op, err_msg, false);
}

int RGWAccessKeyPool::add(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  int ret; 
  std::string subprocess_msg;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse request, " + subprocess_msg);
    return ret;
  }

  ret = execute_add(op, &subprocess_msg, defer_user_update);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to add access key, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWAccessKeyPool::execute_remove(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  int ret = 0;

  int key_type = op.get_key_type();
  std::string id = op.get_access_key();
  map<std::string, RGWAccessKey>::iterator kiter;
  map<std::string, RGWAccessKey> *keys_map;

  if (!op.has_existing_key()) {
    set_err_msg(err_msg, "unable to find access key");
    return -EINVAL;
  }

  if (key_type == KEY_TYPE_S3) {
    keys_map = access_keys;
  } else if (key_type == KEY_TYPE_SWIFT) {
    keys_map = swift_keys;
  } else {
    keys_map = NULL;
    set_err_msg(err_msg, "invalid access key");
    return -EINVAL;
  }

  kiter = keys_map->find(id);
  if (kiter == keys_map->end()) {
    set_err_msg(err_msg, "key not found");
    return -EINVAL;
  }

  rgw_remove_key_index(store, kiter->second);
  keys_map->erase(kiter);

  if (!defer_user_update)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWAccessKeyPool::remove(RGWUserAdminOperation& op, std::string *err_msg)
{
  return remove(op, err_msg, false);
}

int RGWAccessKeyPool::remove(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  int ret;

  std::string subprocess_msg;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse request, " + subprocess_msg);
    return ret;
  }

  ret = execute_remove(op, &subprocess_msg, defer_user_update);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to remove access key, " + subprocess_msg);
    return ret;
  }

  return 0;
}

RGWSubUserPool::RGWSubUserPool(RGWUser *usr)
{
   if (!usr || usr->failure)
    subusers_allowed = false;

  subusers_allowed = true;

  store = usr->get_store();
  user = usr;
}

RGWSubUserPool::~RGWSubUserPool()
{

}

int RGWSubUserPool::init(RGWUserAdminOperation& op)
{
  if (!op.is_initialized()) {
    subusers_allowed = false;
    return -EINVAL;
  }

  std::string uid = op.get_user_id();
  if (uid.compare(RGW_USER_ANON_ID) == 0) {
    subusers_allowed = false;
    return -EACCES;
  }

  subuser_map = op.get_subusers();
  if (subuser_map == NULL) {
    subusers_allowed = false;
    return -EINVAL;
  }

  subusers_allowed = true;

  return 0;
}

bool RGWSubUserPool::exists(std::string subuser)
{
  if (subuser.empty())
    return false;

  if (!subuser_map)
    return false;

  if (subuser_map->count(subuser))
    return true;

  return false;
}

int RGWSubUserPool::check_op(RGWUserAdminOperation& op,
        std::string *err_msg)
{
  bool existing = false;
  string subprocess_msg;
  std::string subuser = op.get_subuser();

  if (!op.is_populated()) {
    set_err_msg(err_msg, "user info was not populated");
    return -EINVAL;
  }

  if (!subusers_allowed) {
    set_err_msg(err_msg, "subusers not allowed for this user");
    return -EACCES;
  }

  if (subuser.empty() && !op.will_gen_subuser()) {
    set_err_msg(err_msg, "empty subuser name");
    return -EINVAL;
  }

  // check if the subuser exists
  if (!subuser.empty())
    existing = exists(subuser);

  if (existing)
    op.set_existing_subuser();

  return 0;
}

int RGWSubUserPool::execute_add(RGWUserAdminOperation& op,
        std::string *err_msg, bool defer_user_update)
{
  int ret = 0;
  std::string subprocess_msg;

  RGWSubUser subuser;
  std::pair<std::string, RGWSubUser> subuser_pair;
  std::string subuser_str = op.get_subuser();

  subuser_pair.first = subuser_str;

  // no duplicates
  if (op.has_existing_subuser()) {
    set_err_msg(err_msg, "subuser exists");
    return -EEXIST;
  }

  // assumes key should be created
  if (op.has_key_op()) {
    ret = user->keys->add(op, &subprocess_msg, true);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to create subuser key, " + subprocess_msg);
      return ret;
    }
  }

  // create the subuser
  subuser.name = subuser_str;

  if (op.has_subuser_perm())
    subuser.perm_mask = op.get_subuser_perm();

  // insert the subuser into user info
  subuser_pair.second = subuser;
  subuser_map->insert(subuser_pair);

  // attempt to save the subuser
  if (!defer_user_update)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWSubUserPool::add(RGWUserAdminOperation& op, std::string *err_msg)
{
  return add(op, err_msg, false);
}

int RGWSubUserPool::add(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  std::string subprocess_msg;
  int ret;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse request, " + subprocess_msg);
    return ret;
  }

  ret = execute_add(op, &subprocess_msg, defer_user_update);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to create subuser, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWSubUserPool::execute_remove(RGWUserAdminOperation& op,
        std::string *err_msg, bool defer_user_update)
{
  int ret = 0;
  std::string subprocess_msg;

  std::string subuser_str = op.get_subuser();

  map<std::string, RGWSubUser>::iterator siter;
  siter = subuser_map->find(subuser_str);

  if (!op.has_existing_subuser()) {
    set_err_msg(err_msg, "subuser not found: " + subuser_str);
    return -EINVAL;
  }

  if (op.will_purge_keys()) {
    ret = user->keys->remove(op, &subprocess_msg, true);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to remove subuser keys, " + subprocess_msg);
      return ret;
    }
  }

  //remove the subuser from the user info
  subuser_map->erase(siter);

  // attempt to save the subuser
  if (!defer_user_update)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWSubUserPool::remove(RGWUserAdminOperation& op, std::string *err_msg)
{
  return remove(op, err_msg, false);
}

int RGWSubUserPool::remove(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  std::string subprocess_msg;
  int ret;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse request, " + subprocess_msg);
    return ret;
  }

  ret = execute_remove(op, &subprocess_msg, defer_user_update);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to remove subuser, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWSubUserPool::execute_modify(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  int ret = 0;
  std::string subprocess_msg;
  std::map<std::string, RGWSubUser>::iterator siter;
  std::pair<std::string, RGWSubUser> subuser_pair;

  std::string subuser_str = op.get_subuser();
  RGWSubUser subuser;

  if (!op.has_existing_subuser()) {
    set_err_msg(err_msg, "subuser does not exist");
    return -EINVAL;
  }

  subuser_pair.first = subuser_str;

  siter = subuser_map->find(subuser_str);
  subuser = siter->second;

  if (op.has_key_op()) {
    ret = user->keys->add(op, &subprocess_msg, true);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to create subuser keys, " + subprocess_msg);
      return ret;
    }
  }

  if (op.has_subuser_perm())
    subuser.perm_mask = op.get_subuser_perm();

  subuser_pair.second = subuser;

  subuser_map->erase(siter);
  subuser_map->insert(subuser_pair);

  // attempt to save the subuser
  if (!defer_user_update)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWSubUserPool::modify(RGWUserAdminOperation& op, std::string *err_msg)
{
  return RGWSubUserPool::modify(op, err_msg, false);
}

int RGWSubUserPool::modify(RGWUserAdminOperation& op, std::string *err_msg, bool defer_user_update)
{
  std::string subprocess_msg;
  int ret;

  RGWSubUser subuser;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse request, " + subprocess_msg);
    return ret;
  }

  ret = execute_modify(op, &subprocess_msg, defer_user_update);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to modify subuser, " + subprocess_msg);
    return ret;
  }

  return 0;
}

RGWUserCapPool::RGWUserCapPool(RGWUser *usr)
{
  if (!user || usr->has_failed()) {
    caps_allowed = false;
  }

  user = usr;
}

RGWUserCapPool::~RGWUserCapPool()
{

}

int RGWUserCapPool::init(RGWUserAdminOperation& op)
{
  if (!op.is_initialized()) {
    caps_allowed = false;
    return -EINVAL;
  }

  std::string uid = op.get_user_id();
  if (uid == RGW_USER_ANON_ID) {
    caps_allowed = false;
    return -EACCES;
  }

  caps = op.get_caps_obj();
  if (!caps) {
    caps_allowed = false;
    return -EINVAL;
  }

  caps_allowed = true;

  return 0;
}

int RGWUserCapPool::add(RGWUserAdminOperation& op, std::string *err_msg)
{
  return add(op, err_msg, false);
}

int RGWUserCapPool::add(RGWUserAdminOperation& op, std::string *err_msg, bool defer_save)
{
  int ret = 0;
  std::string subprocess_msg;
  std::string caps_str = op.get_caps();

  if (!op.is_populated()) {
    set_err_msg(err_msg, "user info was not populated");
    return -EINVAL;
  }

  if (!caps_allowed) {
    set_err_msg(err_msg, "caps not allowed for this user");
    return -EACCES;
  }

  if (caps_str.empty()) {
    set_err_msg(err_msg, "empty user caps");
    return -EINVAL;
  }

  int r = caps->add_from_string(caps_str);
  if (r < 0) {
    set_err_msg(err_msg, "unable to add caps: " + caps_str);
    return r;
  }

  if (!defer_save)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

int RGWUserCapPool::remove(RGWUserAdminOperation& op, std::string *err_msg)
{
  return remove(op, err_msg, false);
}

int RGWUserCapPool::remove(RGWUserAdminOperation& op, std::string *err_msg, bool defer_save)
{
  int ret = 0;
  std::string subprocess_msg;

  std::string caps_str = op.get_caps();

  if (!op.is_populated()) {
    set_err_msg(err_msg, "user info was not populated");
    return -EINVAL;
  }

  if (!caps_allowed) {
    set_err_msg(err_msg, "caps not allowed for this user");
    return -EACCES;
  }

  if (caps_str.empty()) {
    set_err_msg(err_msg, "empty user caps");
    return -EINVAL;
  }

  int r = caps->remove_from_string(caps_str);
  if (r < 0) {
    set_err_msg(err_msg, "unable to remove caps: " + caps_str);
    return r;
  }

  if (!defer_save)
    ret = user->update(op, err_msg);

  if (ret < 0)
    return ret;

  return 0;
}

RGWUser::RGWUser()
{
  init_default();
}

RGWUser::RGWUser(RGWRados *storage)
{
  init_default();

  if (init_storage(storage) < 0)
    set_failure();
}

RGWUser::RGWUser(RGWRados *storage, RGWUserAdminOperation& op)
{
  init_default();

  if (init_storage(storage) < 0) {
    set_failure();
    return;
  }

  if (init(op) < 0)
    set_failure();
}

RGWUser::~RGWUser()
{
  clear_members();
}

void RGWUser::clear_members()
{
  if (keys != NULL)
    delete keys;

  if (caps != NULL)
    delete caps;

  if (subusers != NULL)
    delete subusers;
}

void RGWUser::init_default()
{
  // use anonymous user info as a placeholder
  rgw_get_anon_user(old_info);
  user_id = RGW_USER_ANON_ID;

  clear_failure();
  clear_populated();

  keys = NULL;
  caps = NULL;
  subusers = NULL;
}

int RGWUser::init_storage(RGWRados *storage)
{
  if (!storage) {
    set_failure();
    return -EINVAL;
  }

  store = storage;

  clear_failure();
  clear_populated();

  /* API wrappers */
  keys = new RGWAccessKeyPool(this);
  caps = new RGWUserCapPool(this);
  subusers = new RGWSubUserPool(this);

  return 0;
}

int RGWUser::init(RGWUserAdminOperation& op)
{
  bool found = false;
  std::string swift_user;
  std::string uid = op.get_user_id();
  std::string user_email = op.get_user_email();
  std::string access_key = op.get_access_key();

  int key_type = op.get_key_type();
  if (key_type == KEY_TYPE_SWIFT) {
    swift_user = op.get_access_key();
    access_key.clear();
  }

  RGWUserInfo user_info;

  clear_populated();
  clear_failure();

  if (!uid.empty() && (uid.compare(RGW_USER_ANON_ID) != 0))
    found = (rgw_get_user_info_by_uid(store, uid, user_info) >= 0);

  if (!user_email.empty() && !found)
    found = (rgw_get_user_info_by_email(store, user_email, user_info) >= 0);

  if (!swift_user.empty() && !found)
    found = (rgw_get_user_info_by_swift(store, swift_user, user_info) >= 0);

  if (!access_key.empty() && !found)
    found = (rgw_get_user_info_by_access_key(store, access_key, user_info) >= 0);

  if (found) {
    op.set_existing_user();
    op.set_user_info(user_info);
    op.set_populated();

    old_info = user_info;
    set_populated();
  }

  user_id = user_info.user_id;
  op.set_initialized();

  // this may have been called by a helper object
  int ret = init_members(op);
  if (ret < 0)
    return ret;

  return 0;
}

int RGWUser::init_members(RGWUserAdminOperation& op)
{
  int ret = 0;

  if (!keys || !subusers || !caps)
    return -EINVAL;

  ret = keys->init(op);
  if (ret < 0)
    return ret;

  ret = subusers->init(op);
  if (ret < 0)
    return ret;

  ret = caps->init(op);
  if (ret < 0)
    return ret;

  return 0;
}

int RGWUser::update(RGWUserAdminOperation& op, std::string *err_msg)
{
  int ret;
  std::string subprocess_msg;
  RGWUserInfo user_info = op.get_user_info();

  if (!store) {
    set_err_msg(err_msg, "couldn't initialize storage");
    return -EINVAL;
  }

  if (is_populated()) {
    ret = rgw_store_user_info(store, user_info, &old_info, false);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to store user info");
      return ret;
    }

    ret = remove_old_indexes(store, old_info, user_info, &subprocess_msg);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to remove old user info, " + subprocess_msg);
      return ret;
    }
  } else {
    ret = rgw_store_user_info(store, user_info, NULL, false);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to store user info");
      return ret;
    }
  }

  old_info = user_info;
  set_populated();

  return 0;
}

int RGWUser::check_op(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string subprocess_msg;
  bool same_id;
  bool populated;
  bool existing_email = false;
  std::string op_id = op.get_user_id();
  std::string op_email = op.get_user_email();

  RGWUserInfo user_info;

  same_id = (user_id.compare(op_id) == 0);
  populated = is_populated();

  if (op_id.compare(RGW_USER_ANON_ID) == 0) {
    set_err_msg(err_msg, "unable to perform operations on the anoymous user");
    return -EINVAL;
  }

  if (populated && !same_id) {
    set_err_msg(err_msg, "user id mismatch, operation id: " + op_id\
            + " does not match: " + user_id);

    return -EINVAL;
  }

  // check for an existing user email
  if (!op_email.empty())
    existing_email = (rgw_get_user_info_by_email(store, op_email, user_info) >= 0);

  if (existing_email)
    op.set_existing_email();

  return 0;
}

int RGWUser::execute_add(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string subprocess_msg;
  int ret = 0;
  bool defer_user_update = true;

  RGWUserInfo user_info;

  std::string uid = op.get_user_id();
  std::string user_email = op.get_user_email();
  std::string display_name = op.get_display_name();

  // fail if the user exists already
  if (op.has_existing_user()) {
    set_err_msg(err_msg, "user: " + op.user_id + " exists");
    return -EEXIST;
  }

  // fail if the user_info has already been populated
  if (op.is_populated()) {
    set_err_msg(err_msg, "cannot overwrite already populated user");
    return -EEXIST;
  }

  // fail if the display name was not included
  if (display_name.empty()) {
    set_err_msg(err_msg, "no display name specified");
    return -EINVAL;
  }

  // fail if the user email is a duplicate
  if (op.has_existing_email()) {
    set_err_msg(err_msg, "duplicate email provided");
    return -EEXIST;
  }

  // set the user info
  user_id = uid;
  user_info.user_id = user_id;
  user_info.display_name = display_name;

  if (!user_email.empty())
    user_info.user_email = user_email;

  user_info.max_buckets = op.get_max_buckets();
  user_info.suspended = op.get_suspension_status();

  // update the request
  op.set_user_info(user_info);
  op.set_populated();

  // update the helper objects
  ret = init_members(op);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to initialize user");
    return ret;
  }

  // see if we need to add an access key
  if (op.has_key_op()) {
    ret = keys->add(op, &subprocess_msg, defer_user_update);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to create access key, " + subprocess_msg);
      return ret;
    }
  }

  // see if we need to add some caps
  if (op.has_caps_op()) {
    ret = caps->add(op, &subprocess_msg, defer_user_update);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to add user capabilities, " + subprocess_msg);
      return ret;
    }
  }

  ret = update(op, err_msg);
  if (ret < 0)
    return ret;

  return 0;
}

int RGWUser::add(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string subprocess_msg;
  int ret;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse parameters, " + subprocess_msg);
    return ret;
  }

  ret = execute_add(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to create user, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWUser::execute_remove(RGWUserAdminOperation& op, std::string *err_msg)
{
  int ret;

  bool purge_data = op.will_purge_data();
  std::string uid = op.get_user_id();
  RGWUserInfo user_info = op.get_user_info();

  if (!op.has_existing_user()) {
    set_err_msg(err_msg, "user does not exist");
    return -EINVAL;
  }

  RGWUserBuckets buckets;
  ret = rgw_read_user_buckets(store, uid, buckets, false);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to read user bucket info");
    return ret;
  }

  map<std::string, RGWBucketEnt>& m = buckets.get_buckets();
  if (!m.empty() && !purge_data) {
    set_err_msg(err_msg, "must specify purge data to remove user with buckets");
    return -EEXIST; // change to code that maps to 409: conflict
  }

  if (!m.empty()) {
    std::map<std::string, RGWBucketEnt>::iterator it;
    for (it = m.begin(); it != m.end(); it++) {
      ret = remove_bucket(store, ((*it).second).bucket, true);
      if (ret < 0) {
        set_err_msg(err_msg, "unable to delete user data");
        return ret;
      }
    }
  }

  ret = rgw_delete_user(store, user_info);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to remove user from RADOS");
    return ret;
  }

  return 0;
}

int RGWUser::remove(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string subprocess_msg;
  int ret;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse parameters, " + subprocess_msg);
    return ret;
  }

  ret = execute_remove(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to remove user, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWUser::execute_modify(RGWUserAdminOperation& op, std::string *err_msg)
{
  bool same_email = true;
  bool populated = op.is_populated();
  bool defer_user_update = true;
  int ret = 0;

  std::string subprocess_msg;
  std::string op_email = op.get_user_email();
  std::string display_name = op.get_display_name();

  RGWUserInfo user_info;
  RGWUserInfo duplicate_check;

  // ensure that the user info has been populated or is populate-able
  if (!op.has_existing_user() && !populated) {
    set_err_msg(err_msg, "user not found");
    return -EINVAL;
  }

  // if the user hasn't already been populated...attempt to
  if (!populated) {
    ret = init(op);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to retrieve user info");
      return ret;
    }
  }

  // ensure that we can modify the user's attributes
  if (user_id == RGW_USER_ANON_ID) {
    set_err_msg(err_msg, "unable to modify anonymous user's info");
    return -EACCES;
  }

  user_info = old_info;

  std::string old_email = old_info.user_email;
  if (!old_email.empty())
    same_email = (old_email.compare(op_email) == 0);

  // make sure we are not adding a duplicate email
  if (!op_email.empty() && !same_email) {
    ret = rgw_get_user_info_by_email(store, op_email, duplicate_check);
    if (ret >= 0) {
      set_err_msg(err_msg, "cannot add duplicate email");
      return -EEXIST;
    }

    user_info.user_email = op_email;
  }

  // update the remaining user info
  if (!display_name.empty())
    user_info.display_name = display_name;

  // will be set to RGW_DEFAULT_MAX_BUCKETS by default
  uint32_t max_buckets = op.get_max_buckets();

  if (max_buckets != RGW_DEFAULT_MAX_BUCKETS)
    user_info.max_buckets = max_buckets;

  if (op.has_suspension_op()) {
    __u8 suspended = op.get_suspension_status();
    user_info.suspended = suspended;

    RGWUserBuckets buckets;

    if (user_id.empty()) {
      set_err_msg(err_msg, "empty user id passed...aborting");
      return -EINVAL;
    }

    ret = rgw_read_user_buckets(store, user_id, buckets, false);
    if (ret < 0) {
      set_err_msg(err_msg, "could not get buckets for uid:  " + user_id);
      return ret;
    }

    map<string, RGWBucketEnt>& m = buckets.get_buckets();
    map<string, RGWBucketEnt>::iterator iter;

    vector<rgw_bucket> bucket_names;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      RGWBucketEnt obj = iter->second;
      bucket_names.push_back(obj.bucket);
    }

    ret = store->set_buckets_enabled(bucket_names, !suspended);
    if (ret < 0) {
      set_err_msg(err_msg, "failed to change pool");
      return ret;
    }
  }

  // if we're supposed to modify keys, do so
  if (op.has_key_op()) {
    ret = keys->add(op, &subprocess_msg, defer_user_update);
    if (ret < 0) {
      set_err_msg(err_msg, "unable to create or modify keys, " + subprocess_msg);
      return ret;
    }
  }

  op.set_user_info(user_info);

  ret = update(op, err_msg);
  if (ret < 0)
    return ret;

  return 0;
}

int RGWUser::modify(RGWUserAdminOperation& op, std::string *err_msg)
{
  std::string subprocess_msg;
  int ret;

  ret = check_op(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to parse parameters, " + subprocess_msg);
    return ret;
  }

  ret = execute_modify(op, &subprocess_msg);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to modify user, " + subprocess_msg);
    return ret;
  }

  return 0;
}

int RGWUser::info(RGWUserAdminOperation& op, RGWUserInfo& fetched_info, std::string *err_msg)
{
  int ret = init(op);
  if (ret < 0) {
    set_err_msg(err_msg, "unable to fetch user info");
    return ret;
  }

  // return the user info
  fetched_info = op.get_user_info();

  return 0;
}

int RGWUser::info(RGWUserInfo& fetched_info, std::string *err_msg)
{
  if (!is_populated()) {
    set_err_msg(err_msg, "no user info saved");
    return -EINVAL;
  }

  if (failure) {
   set_err_msg(err_msg, "previous error detected...aborting");
   return -1; // should map to 500 error
  }

  // return the user info
  fetched_info = old_info;

  return 0;
}
