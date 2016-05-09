#!/bin/bash

UUID=`uuidgen`
AMOUNT=$1
COMMAND="{ \"cmd\":\"last-reject-note\", \"msgId\":\"${UUID}\" }"

redis-cli publish validator-request "${COMMAND}" 
