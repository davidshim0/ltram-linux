# Yahoo! Cloud System Benchmark
# Fake small workload
#   Read/update ratio: 90/10
#   Default data size: 1KB records (10 fields, 100 bytes each, plus key)
#   Request distribution: uniform

# Connection details
redis.host=127.0.0.1
redis.port=6379
host=127.0.0.1
port=6379

# Workload size, 100KB * 500_000 = 0.5GB
recordcount=500000
operationcount=10000
fieldcount=10
fieldlength=100

# Required for some C++ YCSB versions
dbname=redis

readallfields=true

readproportion=0.9
updateproportion=0.1
scanproportion=0
insertproportion=0

requestdistribution=uniform

# Required Table Name
table=usertable

# Additional fields often required by C++ Map lookups
dotransactions=true
