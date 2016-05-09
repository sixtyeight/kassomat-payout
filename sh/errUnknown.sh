#!/bin/bash

UUID=`uuidgen`
COMMAND="{ \"cmd\":\"foo\",\"msgId\":\"${UUID}\" }"

redis-cli publish hopper-request "${COMMAND}" 
