#!/bin/bash

UUID=`uuidgen`
CHANNELS=$1
COMMAND="{ \"cmd\":\"inhibit-channels\", \"msgId\":\"${UUID}\", \"channels\":\"${CHANNELS}\" }"

redis-cli publish validator-request "${COMMAND}" 
