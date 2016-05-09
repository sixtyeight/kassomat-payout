#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"get-dataset-version\", \"msgId\":\"${UUID}\" }"

redis-cli publish validator-request "${COMMAND}" 
