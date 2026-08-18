# Yahoo! Cloud System Benchmark
# Workload C: Read only
#   Application example: user profile cache, where profiles are constructed elsewhere (e.g., Hadoop)
#                        
#   Read/update ratio: 100/0
#   Default data size: 1 KB records (10 fields, 100 bytes each, plus key)
#   Request distribution: zipfian

# Connection details
redis.host=127.0.0.1
redis.port=6379
host=127.0.0.1
port=6379

# Workload size for the 4GB DRAM "Memory Wall"
recordcount=3200000
operationcount=10000000
fieldcount=10
fieldlength=100

# Required for some C++ YCSB versions
dbname=redis

readallfields=true

readproportion=1
updateproportion=0
scanproportion=0
insertproportion=0

requestdistribution=zipfian

# Required Table Name
table=usertable

# Additional fields often required by C++ Map lookups
dotransactions=true