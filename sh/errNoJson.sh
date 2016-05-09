#!/bin/bash

UUID=`uuidgen`
COMMAND="<changeomatic><command cmd="foo" /></changeomatic>"

redis-cli publish hopper-request "${COMMAND}" 
