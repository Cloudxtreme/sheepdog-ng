/*
 * Copyright (C) 2013 Zelin.io
 *
 * Kai Zhang <kyle@zelin.io>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __MOCK_H__
#define __MOCK_H__

#include <string.h>

#include "util.h"
#include "rbtree.h"

struct mock_method {
	const char *name;
	int nr_call;
	struct rb_node rb;
};

static inline int mock_cmp(const struct mock_method *m1,
			   const struct mock_method *m2)
{
	return strcmp(m1->name, m2->name);
}

extern struct rb_root mock_methods;
#define method_register(m)						\
	static void __attribute__((constructor)) regist_##m(void)	\
	{								\
		rb_insert(&mock_methods, &m, rb, mock_cmp);		\
	}

#define MOCK_VOID_METHOD(m, ...)			\
	static struct mock_method _##m = {		\
		.name = #m,				\
	};						\
	void m(__VA_ARGS__)				\
	{						\
		_##m.nr_call++;				\
			return;				\
	}						\
	method_register(_##m)

#define MOCK_METHOD(m, rt, rv, ...)			\
	static struct mock_method _##m = {		\
		.name = #m,				\
	};						\
	rt m(__VA_ARGS__)				\
	{						\
		_##m.nr_call++;				\
			return rv;			\
	}						\
	method_register(_##m)

int __method_nr_call(const char *name);
void __method_reset_all(void);

#define method_nr_call(m) __method_nr_call(#m)
#define method_reset_all() __method_reset_all()

#endif
