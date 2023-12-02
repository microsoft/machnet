#!/bin/bash
# for loop to run the experiment 10 times
for i in {1..10}
do
    # run the experiment
    sudo GLOG_logtostderr=1 ./faster_tcp_client -local_ip 10.0.255.102 -remote_ip 10.0.255.100  -num_keys 10000000 -active_generator &
done

wait
