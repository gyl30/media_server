#!/bin/bash
# smoke.sh <unpacked package dir> <address> <work dir>: starts the packaged media_server and signaling on non-default ports of <address>,
# checks argument rejection, listeners, the control and signaling HTTP APIs, then stops both with SIGTERM and checks exit codes,
# released listeners and leftover processes. Run it inside an isolated network namespace that owns <address>.
set -u
package=$(readlink -f "$1")
address=$2
work=$(readlink -m "$3")
mkdir -p "$work"
token=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
failures=0
check()
{
    if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; failures=$((failures + 1)); fi
}
listening()
{
    ss -Hltn "sport = :$1" | grep -q . || ss -Hlun "sport = :$1" | grep -q .
}
http_code()
{
    curl -s -o /dev/null -m 3 -w '%{http_code}' "$@"
}

check "rejects --threads 0" "! '$package/bin/media_server' --threads 0 > /dev/null 2>&1"
check "rejects wildcard --bind-address" "! '$package/bin/media_server' --bind-address 0.0.0.0 > /dev/null 2>&1"
check "rejects unknown option" "! '$package/bin/media_server' --no-such-option > /dev/null 2>&1"

"$package/bin/media_server" --bind-address "$address" --webrtc-address "$address" --rtmp-port 41935 --rtsp-port 48554 --http-port 48080 \
    --threads 2 --signaling-url "http://$address:49090" --control-token "$token" > "$work/media.log" 2>&1 &
media=$!
"$package/bin/signaling" -database "$work/signaling.db" -http-listen "$address:49090" -sip-listen "$address:45060" -sip-advertise "$address:45060" \
    -media-control-url "http://$address:48080" -media-control-token "$token" -media-ip "$address" -media-http-port 48080 \
    -media-rtmp-port 41935 -media-rtsp-port 48554 > "$work/signaling.log" 2>&1 &
signaling=$!
for _ in $(seq 1 100); do
    listening 48080 && listening 49090 && break
    sleep 0.1
done

check "media_server first log line names the build" "head -1 '$work/media.log' | grep -qF \"\$('$package/bin/media_server' --version)\""
for port in 41935 48554 48080 49090 45060; do
    check "listening on $port" "listening $port"
done
check "control API with token" "[ \"\$(http_code -H 'Authorization: Bearer $token' http://$address:48080/receivers)\" = 200 ]"
check "control API rejects a missing token" "[ \"\$(http_code http://$address:48080/receivers)\" = 403 ]"
check "control API rejects a wrong token" "[ \"\$(http_code -H 'Authorization: Bearer wrong' http://$address:48080/receivers)\" = 403 ]"
check "receivers list is empty" "curl -s -m 3 -H 'Authorization: Bearer $token' http://$address:48080/receivers | grep -q '\"receivers\":\[\]'"
check "signaling web console" "[ \"\$(http_code http://$address:49090/)\" = 200 ]"
check "signaling device API" "[ \"\$(http_code http://$address:49090/api/devices)\" = 200 ]"
check "signaling database created" "[ -s '$work/signaling.db' ]"

kill -TERM "$signaling" "$media"
wait "$signaling"; signaling_exit=$?
wait "$media"; media_exit=$?
check "signaling exits 0 on SIGTERM (got $signaling_exit)" "[ $signaling_exit = 0 ]"
check "media_server exits 0 on SIGTERM (got $media_exit)" "[ $media_exit = 0 ]"
for port in 41935 48554 48080 49090 45060; do
    check "port $port released" "! listening $port"
done
check "no leftover processes" "! pgrep -f '$package/bin/' > /dev/null"
check "no error lines in media log" "! grep -iE '\[(error|critical)\]' '$work/media.log'"
echo "media log tail:"; tail -3 "$work/media.log"
[ $failures = 0 ] && echo PASS || echo FAIL
exit $((failures != 0))
