#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"smart-empty\", \"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
