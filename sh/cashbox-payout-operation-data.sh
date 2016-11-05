#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"cashbox-payout-operation-data\", \"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
