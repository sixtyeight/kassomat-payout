#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"empty\", \"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
