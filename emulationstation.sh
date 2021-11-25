#!/bin/sh

esdir="$(dirname $0)"

echo "current dir: $esdir"
cd $esdir

while true; do
    rm -f /tmp/es-restart /tmp/es-sysrestart /tmp/es-shutdown
    ./emulationstation "$@"
    ret=$?
    [ -f /tmp/es-restart ] && continue
    if [ -f /tmp/es-sysrestart ]; then
        rm -f /tmp/es-sysrestart
        systemctl reboot
        break
    fi
    if [ -f /tmp/es-shutdown ]; then
        rm -f /tmp/es-shutdown
        systemctl poweroff
        break
    fi
    break
done
exit $ret
