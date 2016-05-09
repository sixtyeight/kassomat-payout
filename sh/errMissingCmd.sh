#!/bin/bash

UUID=`uuidgen`
COMMAND="{ \"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
