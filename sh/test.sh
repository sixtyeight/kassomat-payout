#!/bin/bash

UUID=`uuidgen`
COMMAND="{ \"cmd\":\"test\",\"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
