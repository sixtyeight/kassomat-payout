#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"test-payout\", \"msgId\":\"${UUID}\", \"amount\":${AMOUNT} }"

redis-cli publish hopper-request "${COMMAND}" 
