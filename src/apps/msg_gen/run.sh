for i in {1..200}
do
   sudo GLOG_logtostderr=1 ./msg_gen --local_ip 10.0.255.100 --remote_ip 10.0.255.102 -msg_window 1 -active_generator
done
