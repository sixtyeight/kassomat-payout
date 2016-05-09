#!/bin/bash

UUID=`uuidgen`
COMMAND="{ \"cmd\":\"quit\",\"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
