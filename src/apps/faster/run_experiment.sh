#!/bin/bash

cd ~/machnet/build/src/apps/faster

apps=$1 # number of apps
local_ip=$2 # local ip

for i in $(seq 0 $apps)
do
    # run the experiment
    echo $i
    sudo ./faster_tcp_client -local_ip $2 -remote_ip 10.0.255.100  -num_keys 10000000 -active_generator &
done

# wait
