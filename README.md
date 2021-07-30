HERD
====

you can see introduction on original page [efficient/HERD (github.com)](https://github.com/efficient/HERD)

### to run my version, you need:

1. make sure the network environment support ROCE (IB not tested)
2. make sure the port start from 13000 on the server machine is available, you can modify it in ./run.sh
3. change the cpu-core-binding in function main() for server 
4. make sure you have enough shared memory and hugepages (page of 2M or 1G, any is OK)
5. run "sudo ./run.sh" on server machine, after that, run "sudo ./run.sh id IP" on each client machine, note that "id" is the client machine's number, start from 0, and "IP" is the server IP address.
6. If you want to change the number of server processes, client machines, or client processes on each client machine, you need to change ./run.sh and ./num.h
7. to kill the processes, run "sudo ./kill_all.sh" on every machine