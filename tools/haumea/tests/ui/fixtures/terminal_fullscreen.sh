#!/bin/sh
# The alternate screen, as a full-screen program uses it.
printf '\033[?1049h\033[2J\033[1;1HFULL%s' SCREEN
sleep 2
printf '\033[?1049l'
