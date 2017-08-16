#!/bin/bash

# This is supposed to be run only via qemu.sh.

TELNET_PORT=4444
telnet -e~ localhost $TELNET_PORT
