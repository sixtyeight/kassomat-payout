#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"do-float\", \"msgId\":\"${UUID}\", \"amount\":${AMOUNT} }"

redis-cli publish hopper-request "${COMMAND}" 
