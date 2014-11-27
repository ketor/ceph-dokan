
#include "../include/types.h"
#include "Messenger.h"

#include "../msg/simple/SimpleMessenger.h"
//by ketor #include "../msg/async/AsyncMessenger.h"

Messenger *Messenger::create(CephContext *cct,
			     entity_name_t name,
			     string lname,
			     uint64_t nonce)
{
  int r = -1;
  if (cct->_conf->ms_type == "random")
    r = rand() % 2;
  if (r == 0 || cct->_conf->ms_type == "simple")
    return new SimpleMessenger(cct, name, lname, nonce);
  //by ketor else if (r == 1 || cct->_conf->ms_type == "async")
  //by ketor   return new AsyncMessenger(cct, name, lname, nonce);
  lderr(cct) << "unrecognized ms_type '" << cct->_conf->ms_type << "'" << dendl;
  return NULL;
}
