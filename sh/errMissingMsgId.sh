#!/bin/bash

UUID=`uuidgen`
COMMAND="{ \"cmd\":\"enable\" }"

redis-cli publish hopper-request "${COMMAND}" 
