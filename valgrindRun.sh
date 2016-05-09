#!/bin/bash

export LD_LIBRARY_PATH=.

valgrind --track-origins=yes --leak-check=full --show-leak-kinds=all ./payoutd -d /dev/kassomat 
