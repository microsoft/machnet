#!/bin/bash

for i in {1..40}
do
    # run the experiment
    sudo GLOG_logtostderr=1 ./faster_tcp_client -local_ip 10.0.255.102 -remote_ip 10.0.255.100  -num_keys 10000000 -active_generator &>> output.txt &
done

wait
