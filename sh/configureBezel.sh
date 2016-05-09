#!/bin/bash

UUID=`uuidgen`
R=$1
G=$2
B=$3
T=$4
COMMAND="{ \"cmd\":\"configure-bezel\",\"r\":$R,\"g\":$G,\"b\":$B,\"type\":$T,\"msgId\":\"${UUID}\" }"

redis-cli publish validator-request "${COMMAND}" 
