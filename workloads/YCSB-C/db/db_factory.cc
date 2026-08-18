//
//  basic_db.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/17/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include "db/db_factory.h"

#include <string>
#include "db/basic_db.h"
#include "db/lock_stl_db.h"
#include "db/redis_db.h"
#ifdef USE_TBB
#include "db/tbb_rand_db.h"
#include "db/tbb_scan_db.h"
#endif

using namespace std;
using ycsbc::DB;
using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  const string dbname = props.GetProperty("dbname");
  if (dbname == "basic") {
    return new BasicDB;
  } else if (dbname == "lock_stl") {
    return new LockStlDB;
  } else if (dbname == "redis") {
    int port = stoi(props.GetProperty("port", "6379"));
    int slaves = stoi(props.GetProperty("slaves", "0"));
    return new RedisDB(props.GetProperty("host", "127.0.0.1").c_str(), port, slaves);
#ifdef USE_TBB
  } else if (dbname == "tbb_rand") {
    return new TbbRandDB;
  } else if (dbname == "tbb_scan") {
    return new TbbScanDB;
#endif
  } else return NULL;
}

