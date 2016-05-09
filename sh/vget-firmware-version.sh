#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"get-firmware-version\", \"msgId\":\"${UUID}\" }"

redis-cli publish validator-request "${COMMAND}" 
