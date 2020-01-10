/*
 * Copyright (c) 2006 by Keith Packard
 * Copyright (c) 2017, 2019 Datalight, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Utilities for working with RCS revision numbers.
 */
#include <stdio.h>
#include <string.h>
#include "interfaces.h"

/* are two RCS revision numbers equal? */
bool
rcs_number_equal(const struct rcs_number *n1, const struct rcs_number *n2)
{
	/* can use memcmp as struct rcs_number isn't padded */
	return !memcmp(n1, n2, sizeof(short) * (1 + n1->c));
}

/* is num equal to spec, up through the end of spec? */
bool
rcs_number_partial_match(const struct rcs_number *num,
	const struct rcs_number *spec)
{
	int i;

	if (num->c < spec->c)
		return false;
	for (i = 0; i < spec->c; ++i)
		if (num->n[i] != spec->n[i])
			return false;
	return true;
}

/* total ordering for RCS revision numbers -- parent always < child */
int
rcs_number_compare(const struct rcs_number *a, const struct rcs_number *b)
{
	int i, n;

	/*
	 * On the same branch, earlier commits compare before later ones.  On
	 * different branches of the same degree, the earlier one compares
	 * before the later one.
	 */
	n = min(a->c, b->c);
	for (i = 0; i < n; i++) {
		if (a->n[i] < b->n[i])
			return -1;
		if (a->n[i] > b->n[i])
			return 1;
	}

	/* Branch root commits sort before any commit on their branch. */
	if (a->c < b->c)
		return -1;
	if (a->c > b->c)
		return 1;

	/* Can happen only if the RCS numbers are equal. */
	return 0;
}

/* does the specified CVS release number describe a trunk revision? */
bool
rcs_number_is_trunk(const struct rcs_number *number)
{
	return number->c == 2;
}

/* get the next RCS number */
struct rcs_number *
rcs_number_increment(struct rcs_number *number)
{
	number->n[number->c - 1]++;
	return number;
}

/* get the previous RCS number */
struct rcs_number *
rcs_number_decrement(struct rcs_number *number)
{
	number->n[number->c - 1]--;
	if (number->n[number->c - 1])
		return number;

	if (number->c >= 4) {
		number->c -= 2;
		return number;
	}

	return NULL;
}

/* return the human-readable representation of an RCS release number */
char *
rcs_number_string(const struct rcs_number *n, char *str, size_t maxlen)
{
	char r[RCS_MAX_DIGITS + 1];
	int i;

	if (n == TIP_REVNUM) {
		strncpy(str, "tip", maxlen);
		str[maxlen-1] = '\0';
		goto out;
	}

	str[0] = '\0';
	for (i = 0; i < n->c; i++) {
		snprintf(r, RCS_MAX_DIGITS, "%d", n->n[i]);
		if (i > 0)
			strcat(str, ".");
		if (strlen(str) + strlen(r) < maxlen - 1)
			strcat(str, r);
		else
			fatal_error("revision string too long");
	}
out:
	return str;
}

/* same as rcs_number_string(), but with a static buffer */
const char *
rcs_number_string_sb(const struct rcs_number *n)
{
	static char numstr[RCS_MAX_REV_LEN];
	return rcs_number_string(n, numstr, sizeof numstr);
}
