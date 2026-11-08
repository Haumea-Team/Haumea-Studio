#!/bin/sh
# A program that asks for bracketed paste, and shows what it gets.
printf '\033[?2004h'
cat -v
printf '\033[?2004l'
