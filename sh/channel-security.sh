#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"channel-security\", \"msgId\":\"${UUID}\" }"

redis-cli publish validator-request "${COMMAND}" 
