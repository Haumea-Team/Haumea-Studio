#!/bin/sh
# A program that runs until stopped, and says so once it is (split, so the typed line doesn't match).
printf 'RUNN%s\n' ING
exec sleep 100
