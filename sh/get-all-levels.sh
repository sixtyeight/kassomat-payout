#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"get-all-levels\", \"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
