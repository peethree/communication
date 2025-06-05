#!/bin/bash

build() {
    ./build
}

run_router() {
    ./router
}

# this needs to be run from kitty terminal else it will break
test() {
    ./test.sh
}

# for killing blocked ./dealer client, look up pid
lookup_dealer() {
    ps aux | grep ./dealer 
}

lookup_router() {
    ps aux | grep ./router
}

case "$1" in 
  build|b)
    build
    exit 0
    ;;
  run|r)
    run_router
    exit 0
    ;;
  test|t)
    test
    exit 0
    ;;
  lookd|ld)
    lookup_dealer
    exit 0
    ;;
  lookr|lr)
    lookup_router
    exit 0
    ;;
esac

echo "invalid arg: $1"
    
