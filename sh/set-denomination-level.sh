#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
LEVEL=$2
COMMAND="{ \"cmd\":\"set-denomination-level\", \"msgId\":\"${UUID}\", \"amount\":${AMOUNT}, \"level\":${LEVEL} }"

redis-cli publish hopper-request "${COMMAND}" 
