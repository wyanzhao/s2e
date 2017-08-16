#!/bin/bash

# This is supposed to be run only via qemu.sh
# DON'T RUN THIS UNLESS YOU KNOW WHAT'S GOING ON

# trap "echo Ignoring SIGINT/SIGTERM/SIGHUP" INT TERM HUP

if [ $# -ne 3 ]; then
    echo "Expected three args."
    exit 1
fi

APPCHK=$(ps aux | grep -v "grep" | grep -c "rsync")
if [ "$APPCHK" -ne "0" ]; then
    ps aux | grep -v "grep" | grep "rsync"
    echo "Rsync is running.  Terminate or wait until it's done: $APPCHK."
    exit 2
fi

if [ -e "$3" ]; then
    echo "Backup already exists!?  CONFUSED"
    exit 3
else
    echo "Copying $1 to $2..."
    nice -n 20 ionice -c 3 rsync --bwlimit=10000 $1 $2
    if [ $? -eq 0 ]; then
        echo "Touching $3."
        touch $3
    fi
    echo "Done."
fi

exit 0
