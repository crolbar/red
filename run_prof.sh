#!/usr/bin/env bash
# export RED_DONT_SPAWN_CLIENT=1

export CPUPROFILE=./cpu.prof

./red

pprof -http=:8080 -no_browser ./red "$CPUPROFILE"&
PPROF_PID=$!

chromium http://localhost:8080/ui/graph

kill "$PPROF_PID"
