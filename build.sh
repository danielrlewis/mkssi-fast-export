#!/bin/sh

set -e # Quit if any command fails
set -x # Show each command as it executes

# In case of build failure, we do not want any output files lingering from a
# previous build, as this could cause confusion.
rm -f gram.c gram.h lex.c lex.h mkssi-fast-export

# This takes about one second on my machine, so the time and effort needed to
# create a Makefile does not seem worth it.
bison --defines=gram.h --output-file=gram.c gram.y
flex --header-file=lex.h --outfile=lex.c lex.l
cc -Wall -Wno-unused-function -O2 -march=native -I. -D_GNU_SOURCE \
	authors.c \
	changeset.c \
	export.c \
	gram.c \
	import.c \
	lex.c \
	main.c \
	merge.c \
	project.c \
	rcs.c \
	rcs-number.c \
	utils.c \
	-o mkssi-fast-export
