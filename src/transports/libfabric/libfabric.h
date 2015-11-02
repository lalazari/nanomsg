/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#ifndef NN_LIBFABRIC_INCLUDED
#define NN_LIBFABRIC_INCLUDED

#include "../../transport.h"

extern struct nn_transport *nn_libfabric;

/*******************************************************/

#define INIT_OPTS (struct ft_opts) \
	{	.options = FT_OPT_RX_CQ | FT_OPT_TX_CQ, \
		.iterations = 1000, \
		.transfer_size = 1024, \
		.src_port = "9228", \
		.dst_port = "9228", \
		.argc = argc, .argv = argv \
	}

struct ft_opts {
	int iterations;
	int transfer_size;
	char *src_port;
	char *dst_port;
	char *src_addr;
	char *dst_addr;
	int size_option;
	int options;
	int machr;
	int argc;
	char **argv;
};

extern struct ft_opts opts;
/*******************************************************/


#endif
