# 95/5 read/write over ~700k records. Zipfian, so the hot set stays small
# while the footprint stays large -- the case cold-page tiering handles worst.
recordcount=700000
operationcount=1000000000
workload=com.yahoo.ycsb.workloads.CoreWorkload
readallfields=true
readproportion=0.95
updateproportion=0.05
scanproportion=0
insertproportion=0
requestdistribution=zipfian
fieldcount=10
fieldlength=100
