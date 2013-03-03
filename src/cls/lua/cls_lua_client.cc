#include <errno.h>
#include <string>
#include <vector>
#include "include/encoding.h"
#include "include/rados.h"
#include "include/rados/librados.h"
#include "include/types.h"
#include "cls_lua.h"
#include "cls_lua_client.hpp"

using std::string;
using std::vector;
using librados::IoCtx;
using librados::bufferlist;

namespace cls_lua_client {

  int exec(IoCtx& ioctx, const string& oid, const string& script,
      const string& handler, bufferlist& input, bufferlist& output,
      vector<string> *log)
  {
    struct clslua_cmd cmd;
    struct clslua_reply reply;
    bufferlist outbl, inbl;
    int ret;

    cmd.script = script;
    cmd.funcname = handler;
    cmd.input = input;
    ::encode(cmd, inbl);

    /*
     * TODO: we need to encapsulate the return value as well. for example,
     * -ENOTSUPP is returned if the class is not found, but we also return
     * -ENOTSUPP if a handler isn't found. In the later case we still get a
     * valid reply, in the former not so much.
     */
    ret = ioctx.exec(oid, "lua", "eval", inbl, outbl);
    if (ret < 0)
      return ret;

    try {
      ::decode(reply, outbl);
    } catch (const buffer::error &err) {
      return -EBADMSG;
    }

    output = reply.output;
    if (log)
      log->swap(reply.log);

    return ret;
  }

}
