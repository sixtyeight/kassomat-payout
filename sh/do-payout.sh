#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"do-payout\", \"msgId\":\"${UUID}\", \"amount\":${AMOUNT} }"

redis-cli publish hopper-request "${COMMAND}" 
