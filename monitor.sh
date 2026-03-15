#!/bin/bash

PROCESS="server"
CMD="/home/liu/WebServer/build/server"

while true
do
    PID=$(pgrep -x $PROCESS)

    if [ -z "$PID" ]; then
        echo "$(date) : $PROCESS not running, restarting..."
        $CMD &
    else
        echo "$(date) : $PROCESS running, pid=$PID"
    fi

    sleep 5
done