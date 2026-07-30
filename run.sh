#!/usr/bin/env bash
# export RED_DONT_SPAWN_CLIENT=1

~/toggle_binds.sh
gdb -x debug.gdb --args ./red
~/toggle_binds.sh
