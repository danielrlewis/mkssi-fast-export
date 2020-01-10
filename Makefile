# Copyright (c) 2019 Datalight, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makefile for mkssi-fast-export.
CC=cc
YACC=bison
LEX=flex

CFLAGS=-I. -O2 -march=native -Wall -Werror
# flex/bison generate functions that we don't use, don't warn about them
CFLAGS+=-Wno-unused-function
# needed so that strcasestr() is available
CFLAGS+=-D_GNU_SOURCE

OBJS=\
	authors.o \
	changeset.o \
	export.o \
	gram.o \
	import.o \
	lex.o \
	lines.o \
	main.o \
	merge.o \
	project.o \
	rcs-binary.o \
	rcs-keyword.o \
	rcs-text.o \
	rcs-number.o \
	utils.o

HDRSRC=interfaces.h
HDRGEN=gram.h lex.h
HDR=$(HDRSRC) $(HDRGEN)

.PHONY: all clean

all: mkssi-fast-export

lex.c: lex.l $(HDRSRC)
	$(LEX) --header-file=lex.h --outfile=$@ $<

gram.c: gram.y $(HDRSRC)
	$(YACC) --defines=gram.h --output-file=$@ $<

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

mkssi-fast-export: lex.c gram.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f lex.c lex.h gram.c gram.h *.o mkssi-fast-export
