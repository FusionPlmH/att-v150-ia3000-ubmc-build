#!/bin/sh
# Non-persistent helper used while bringing up ubmc_ipmi_lan on a live box.
#
#   apply-runtime.sh install                 install binary + open udp/623
#   apply-runtime.sh run <bind-list> <acc>   start the daemon with those params
#   apply-runtime.sh log                     show the current daemon log
#   apply-runtime.sh stop                    stop the daemon
#
# <bind-list> is IPMI_LAN_BIND (comma separated interfaces, empty item = wildcard)
# <acc> is written to net.ipv4.udp_l3mdev_accept (0 or 1)

DAEMON=/usr/sbin/ubmc_ipmi_lan
LOG=/tmp/ipmi-lan.log

stop_daemon() {
    killall -q ubmc_ipmi_lan 2>/dev/null
    sleep 1
    return 0
}

do_install() {
    stop_daemon
    if [ -f /tmp/d.b64 ]; then
        base64 -d /tmp/d.b64 > "$DAEMON.new"
        chmod 755 "$DAEMON.new"
        mv -f "$DAEMON.new" "$DAEMON"
    fi
    if [ ! -x "$DAEMON" ]; then
        echo "daemon missing"
        exit 1
    fi
    echo "binary : $(ls -l "$DAEMON" | awk '{print $5}') bytes  md5 $(md5sum "$DAEMON" | cut -d' ' -f1)"

    iptables -D INPUT -i vrf-pub -p udp --dport 623 -j ACCEPT 2>/dev/null
    pos=$(iptables -S INPUT | grep -n -- '-i vrf-pub -j DROP' | head -1 | cut -d: -f1)
    if [ -z "$pos" ]; then
        pos=10
    else
        pos=$((pos - 1))
    fi
    iptables -I INPUT "$pos" -i vrf-pub -p udp --dport 623 -j ACCEPT
    echo "firewall: $(iptables -S INPUT | grep 623)"

    if [ ! -f /config/ipmi-lan.passwd ]; then
        printf '# IPMI over LAN credentials, one user:password per line.\n' > /config/ipmi-lan.passwd
        printf 'root:51L1C0M201B\n' >> /config/ipmi-lan.passwd
        chmod 600 /config/ipmi-lan.passwd
    fi
    echo "passwd : $(wc -l < /config/ipmi-lan.passwd) line(s) in /config/ipmi-lan.passwd"
}

do_run() {
    bind="$1"
    acc="$2"
    reuse="$3"
    stop_daemon
    echo "$acc" > /proc/sys/net/ipv4/udp_l3mdev_accept
    : > "$LOG"
    if [ "$reuse" = "1" ]; then
        env IPMI_LAN_BIND="$bind" IPMI_LAN_DEBUG=1 IPMI_LAN_REUSE=1 \
            nohup "$DAEMON" >> "$LOG" 2>&1 &
    else
        env IPMI_LAN_BIND="$bind" IPMI_LAN_DEBUG=1 \
            nohup "$DAEMON" >> "$LOG" 2>&1 &
    fi
    sleep 2
    echo "--- params ---"
    echo "  IPMI_LAN_BIND='$bind'  udp_l3mdev_accept=$(cat /proc/sys/net/ipv4/udp_l3mdev_accept)  reuse=${reuse:-0}"
    echo "  pids: $(pidof ubmc_ipmi_lan)"
    echo "--- log ---"
    cat "$LOG"
    echo "--- sockets ---"
    grep -i ':026F' /proc/net/udp | sed 's/^/  /'
}

case "$1" in
    install) do_install ;;
    run)     do_run "$2" "$3" "$4" ;;
    log)     cat "$LOG" ;;
    stop)    stop_daemon; echo "stopped" ;;
    *)       echo "usage: $0 install|run <bind> <acc>|log|stop"; exit 1 ;;
esac
