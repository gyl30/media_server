#!/bin/bash
exec strace -f -tt -e trace=sendto,sendmsg -s 16 -o /home/gyl/example/media_server/docs/verification_results/20261023-04334df-final-acceptance/runs/chrome-release-strace.trace /home/gyl/example/media_server/build/media_server "$@"
