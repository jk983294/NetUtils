#!/bin/bash

p3 PostServer.py 18121
p3 PostServer.py 18122
p3 PostServer.py 18123
./balancer/balancer -p 18180 -u localhost:18121,localhost:18122,localhost:18123
./balancer/balancer -p 18180 -u 192.168.2.101:18121,192.168.2.101:18122,192.168.2.101:18123
./balancer/balancer -m random -p 18180 -u 192.168.2.101:18121,192.168.2.101:18122,192.168.2.101:18123

nohup /home/kun/github/NetUtils/cmake-build-debug/balancer/balancer -p 18180 -u localhost:18121,localhost:18122,localhost:18123 2>&1 > /tmp/lb.kun.log &

# client for test
p3 PostClient.py
# client test fail-over
p3 PostClient.py localhost 18180 '/test_empty_response' '{}'
