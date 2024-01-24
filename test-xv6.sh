#!/bin/bash

# python2 is needed to test xv6 (for various pedagogical reasons)
# you can install python2 using the following command:
# sudo apt install python2

cd "$(dirname "$0")"

python2.7 ./tests/run-tests.py --test-path ./tests --project-path ./xv6 $@
