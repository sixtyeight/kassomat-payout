#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"test-float\", \"msgId\":\"${UUID}\", \"amount\":${AMOUNT} }"

redis-cli publish hopper-request "${COMMAND}" 
