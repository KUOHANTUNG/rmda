# rdma rc

## use method
client: ./rcServer -d [device_name] -g [gid] -i [ib_port] [ip_addr]
server:  ./rcServer -d [device_name] -g [gid] -i [ib_port]

## description
first use send/recv exchange addr and rkey
then client read from server memory
last client substitute server memory

## declaration
some expanded function is not working now,
i will solve as soon
