#!/usr/bin/env bash

# Copyright © 2022 Jakub Wilk <jwilk@jwilk.net>
# SPDX-License-Identifier: MIT

set -e -u

here="${0%/*}"
base="$here/.."
prog="$base/ttyjack"

echo 1..1

tmpdir=$(mktemp -d -t ttyjack.test.XXXXXX)

run_tmux()
{
    tmux -f /dev/null -S "$tmpdir/tmux.socket" "$@"
}

quote()
{
    printf '%q' "$1"
}

stat "$prog" > /dev/null
printf '# '
run_tmux -V
run_tmux \
    start-server ';' \
    set-option -g remain-on-exit ';' \
    new-session -x 80 -y 24 -d bash -i
sleep 1
secret=$(LC_ALL=C tr -dc a-z < /dev/urandom | head -c 16)
secret13=$(LC_ALL=C tr a-z n-za-m <<< "$secret")
run_tmux set-buffer $'HISTFILE=/dev/null\n' ';' paste-buffer
run_tmux set-buffer "$(quote "$prog") 'echo cjarq-$secret13 | LC_ALL=C tr a-z n-za-m; exit 0'"$'\n' ';' paste-buffer
sleep 1
out=$(run_tmux capture-pane -p -E 24)
sed -e 's/^/# T> /' <<< "$out"
case $out in
    *"pwned-$secret"*)
        echo ok 1;;
    *)
        echo not ok 1;;
esac
run_tmux kill-session
rm -rf "$tmpdir"

# vim:ts=4 sts=4 sw=4 et ft=sh
