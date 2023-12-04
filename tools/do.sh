#!/bin/bash

IP1="20.117.233.210"
IP2="20.77.12.77"
DIR="~/machnet/"
./rsync.sh $IP1 $DIR
./rsync.sh $IP2 $DIR

