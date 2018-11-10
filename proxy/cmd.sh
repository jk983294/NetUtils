#!/bin/bash

p3 PostServer.py 18121
./proxy/proxy -p 18180 -u localhost:18121

# client for test
p3 PostClient.py 18180
