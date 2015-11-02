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
/***/
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>
#include <netdb.h>

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

#include "shared.h"
#include "pingpong_shared.h"

/***/
#include "blibfabric.h"
#include "alibfabric.h"
#include "../../core/ep.h"

#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/fast.h"
#include "../../utils/int.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
#include <netinet/in.h>
#endif

/*  The backlog is set relatively high so that there are not too many failed
    connection attemps during re-connection storms. */
#define NN_BLIBFABRIC_BACKLOG 100

#define NN_BLIBFABRIC_STATE_IDLE 1
#define NN_BLIBFABRIC_STATE_ACTIVE 2
#define NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRIC 3
#define NN_BLIBFABRIC_STATE_STOPPING_USOCK 4
#define NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRICS 5
#define NN_BLIBFABRIC_STATE_LISTENING 6
#define NN_BLIBFABRIC_STATE_WAITING 7
#define NN_BLIBFABRIC_STATE_CLOSING 8
#define NN_BLIBFABRIC_STATE_STOPPING_BACKOFF 9

#define NN_BLIBFABRIC_SRC_USOCK 1
#define NN_BLIBFABRIC_SRC_ALIBFABRIC 2
#define NN_BLIBFABRIC_SRC_RECONNECT_TIMER 3

struct nn_blibfabric {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  The underlying listening LIBFABRIC socket. */
    struct nn_usock usock;

    /*  The connection being accepted at the moment. */
    struct nn_alibfabric *alibfabric;

    /*  List of accepted connections. */
    struct nn_list alibfabrics;

    /*  Used to wait before retrying to connect. */
    struct nn_backoff retry;
};

/*  nn_epbase virtual interface implementation. */
static void nn_blibfabric_stop (struct nn_epbase *self);
static void nn_blibfabric_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_blibfabric_epbase_vfptr = {
    nn_blibfabric_stop,
    nn_blibfabric_destroy
};

/*  Private functions. */
static void nn_blibfabric_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_blibfabric_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_blibfabric_start_listening (struct nn_blibfabric *self);
static void nn_blibfabric_start_accepting (struct nn_blibfabric *self);


/*Nanomsg-libfabric functions*/
static int run(void);
static int run_test();
static int server_connect_bind(void);
static int client_connect(void);

static char test_name[10] = "custom";
static struct timespec start, end;



/****************
int nn_blibfabric_create (void *hint, struct nn_epbase **epbase)
{
    int rc;
    struct nn_blibfabric *self;
    const char *end;
    const char *pos;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. *//**********
    self = nn_alloc (sizeof (struct nn_blibfabric), "blibfabric");
    alloc_assert (self);

    /*  Initalise the epbase. *//**********
    nn_epbase_init (&self->epbase, &nn_blibfabric_epbase_vfptr, hint);
    addr = nn_epbase_getaddr (&self->epbase);

    /*  Parse the port. *//**********
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    if (nn_slow (!pos)) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    if (nn_slow (rc < 0)) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Check whether IPv6 is to be used. *//**********
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the address. *//**********
    rc = nn_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    if (nn_slow (rc < 0)) {
        nn_epbase_term (&self->epbase);
        return -ENODEV;
    }

    /*  Initialise the structure. *//**********
    nn_fsm_init_root (&self->fsm, nn_blibfabric_handler, nn_blibfabric_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_BLIBFABRIC_STATE_IDLE;
    sz = sizeof (reconnect_ivl);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL,
        &reconnect_ivl, &sz);
    nn_assert (sz == sizeof (reconnect_ivl));
    sz = sizeof (reconnect_ivl_max);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL_MAX,
        &reconnect_ivl_max, &sz);
    nn_assert (sz == sizeof (reconnect_ivl_max));
    if (reconnect_ivl_max == 0)
        reconnect_ivl_max = reconnect_ivl;
    nn_backoff_init (&self->retry, NN_BLIBFABRIC_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    nn_usock_init (&self->usock, NN_BLIBFABRIC_SRC_USOCK, &self->fsm);
    self->alibfabric = NULL;
    nn_list_init (&self->alibfabrics);

    /*  Start the state machine. *//**********
    nn_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. *//**********
    *epbase = &self->epbase;

    return 0;
}
*/
int nn_blibfabric_create (void *hint, struct nn_epbase **epbase)
{
     struct nn_ep *local;
     local=(struct nn_ep*) hint; //Nanomsg addr, provider
     //ep_type *local = (struct nn_ep*)hint;
     

     int op, ret;

     opts = INIT_OPTS;

     hints = fi_allocinfo();
     if (!hints)
	return EXIT_FAILURE;
   printf("Provider inside Libfabric transport is %s", local->provider) ;

	if (!hints->fabric_attr) {
			hints->fabric_attr = malloc(sizeof *(hints->fabric_attr));
			if (!hints->fabric_attr) {
				perror("malloc");
				exit(EXIT_FAILURE);
			}
		}
		hints->fabric_attr->prov_name= strchr (local->provider, ((char)local->provider[2]));
		printf("\n\n Provider free %s", hints->fabric_attr->prov_name) ;
	
		
	
	char testz[20];
	
	//((&opts)->dst_addr)=local->addr; //auto monon stin connect, giati to dst_addr elegxetai gia ean einai connect i bind sto fabtest example
	
	
	hints->ep_attr->type = FI_EP_MSG;
	hints->caps = FI_MSG;
	hints->mode = FI_LOCAL_MR;
	hints->addr_format = FI_SOCKADDR;

	ret = run();

	ft_free_res();
	//return -ret;
     

}


static void nn_blibfabric_stop (struct nn_epbase *self)
{
    struct nn_blibfabric *blibfabric;

    blibfabric = nn_cont (self, struct nn_blibfabric, epbase);

    nn_fsm_stop (&blibfabric->fsm);
}

static void nn_blibfabric_destroy (struct nn_epbase *self)
{
    struct nn_blibfabric *blibfabric;

    blibfabric = nn_cont (self, struct nn_blibfabric, epbase);

    nn_assert_state (blibfabric, NN_BLIBFABRIC_STATE_IDLE);
    nn_list_term (&blibfabric->alibfabrics);
    nn_assert (blibfabric->alibfabric == NULL);
    nn_usock_term (&blibfabric->usock);
    nn_backoff_term (&blibfabric->retry);
    nn_epbase_term (&blibfabric->epbase);
    nn_fsm_term (&blibfabric->fsm);

    nn_free (blibfabric);
}

static void nn_blibfabric_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_blibfabric *blibfabric;
    struct nn_list_item *it;
    struct nn_alibfabric *alibfabric;

    blibfabric = nn_cont (self, struct nn_blibfabric, fsm);

    if (nn_slow (src == NN_FSM_ACTION && type == NN_FSM_STOP)) {
        nn_backoff_stop (&blibfabric->retry);
        if (blibfabric->alibfabric) {
            nn_alibfabric_stop (blibfabric->alibfabric);
            blibfabric->state = NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRIC;
        }
        else {
            blibfabric->state = NN_BLIBFABRIC_STATE_STOPPING_USOCK;
        }
    }
    if (nn_slow (blibfabric->state == NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRIC)) {
        if (!nn_alibfabric_isidle (blibfabric->alibfabric))
            return;
        nn_alibfabric_term (blibfabric->alibfabric);
        nn_free (blibfabric->alibfabric);
        blibfabric->alibfabric = NULL;
        nn_usock_stop (&blibfabric->usock);
        blibfabric->state = NN_BLIBFABRIC_STATE_STOPPING_USOCK;
    }
    if (nn_slow (blibfabric->state == NN_BLIBFABRIC_STATE_STOPPING_USOCK)) {
       if (!nn_usock_isidle (&blibfabric->usock))
            return;
        for (it = nn_list_begin (&blibfabric->alibfabrics);
              it != nn_list_end (&blibfabric->alibfabrics);
              it = nn_list_next (&blibfabric->alibfabrics, it)) {
            alibfabric = nn_cont (it, struct nn_alibfabric, item);
            nn_alibfabric_stop (alibfabric);
        }
        blibfabric->state = NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRICS;
        goto alibfabrics_stopping;
    }
    if (nn_slow (blibfabric->state == NN_BLIBFABRIC_STATE_STOPPING_ALIBFABRICS)) {
        nn_assert (src == NN_BLIBFABRIC_SRC_ALIBFABRIC && type == NN_ALIBFABRIC_STOPPED);
        alibfabric = (struct nn_alibfabric *) srcptr;
        nn_list_erase (&blibfabric->alibfabrics, &alibfabric->item);
        nn_alibfabric_term (alibfabric);
        nn_free (alibfabric);

        /*  If there are no more alibfabric state machines, we can stop the whole
            blibfabric object. */
alibfabrics_stopping:
        if (nn_list_empty (&blibfabric->alibfabrics)) {
            blibfabric->state = NN_BLIBFABRIC_STATE_IDLE;
            nn_fsm_stopped_noevent (&blibfabric->fsm);
            nn_epbase_stopped (&blibfabric->epbase);
            return;
        }

        return;
    }

    nn_fsm_bad_action(blibfabric->state, src, type);
}

static void nn_blibfabric_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_blibfabric *blibfabric;
    struct nn_alibfabric *alibfabric;

    blibfabric = nn_cont (self, struct nn_blibfabric, fsm);

    switch (blibfabric->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_BLIBFABRIC_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_blibfabric_start_listening (blibfabric);
                return;
            default:
                nn_fsm_bad_action (blibfabric->state, src, type);
            }

        default:
            nn_fsm_bad_source (blibfabric->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the alibfabric state machine in this state.         */
/******************************************************************************/
    case NN_BLIBFABRIC_STATE_ACTIVE:
        if (srcptr == blibfabric->alibfabric) {
            switch (type) {
            case NN_ALIBFABRIC_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                nn_list_insert (&blibfabric->alibfabrics, &blibfabric->alibfabric->item,
                    nn_list_end (&blibfabric->alibfabrics));
                blibfabric->alibfabric = NULL;

                /*  Start waiting for a new incoming connection. */
                nn_blibfabric_start_accepting (blibfabric);

                return;

            default:
                nn_fsm_bad_action (blibfabric->state, src, type);
            }
        }

        /*  For all remaining events we'll assume they are coming from one
            of remaining child alibfabric objects. */
        nn_assert (src == NN_BLIBFABRIC_SRC_ALIBFABRIC);
        alibfabric = (struct nn_alibfabric*) srcptr;
        switch (type) {
        case NN_ALIBFABRIC_ERROR:
            nn_alibfabric_stop (alibfabric);
            return;
        case NN_ALIBFABRIC_STOPPED:
            nn_list_erase (&blibfabric->alibfabrics, &alibfabric->item);
            nn_alibfabric_term (alibfabric);
            nn_free (alibfabric);
            return;
        default:
            nn_fsm_bad_action (blibfabric->state, src, type);
        }

/******************************************************************************/
/*  CLOSING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case NN_BLIBFABRIC_STATE_CLOSING:
        switch (src) {

        case NN_BLIBFABRIC_SRC_USOCK:
            switch (type) {
            case NN_USOCK_SHUTDOWN:
                return;
            case NN_USOCK_STOPPED:
                nn_backoff_start (&blibfabric->retry);
                blibfabric->state = NN_BLIBFABRIC_STATE_WAITING;
                return;
            default:
                nn_fsm_bad_action (blibfabric->state, src, type);
            }

        default:
            nn_fsm_bad_source (blibfabric->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-bind is attempted. This way we won't overload           */
/*  the system by continuous re-bind attemps.                                 */
/******************************************************************************/
    case NN_BLIBFABRIC_STATE_WAITING:
        switch (src) {

        case NN_BLIBFABRIC_SRC_RECONNECT_TIMER:
            switch (type) {
            case NN_BACKOFF_TIMEOUT:
                nn_backoff_stop (&blibfabric->retry);
                blibfabric->state = NN_BLIBFABRIC_STATE_STOPPING_BACKOFF;
                return;
            default:
                nn_fsm_bad_action (blibfabric->state, src, type);
            }

        default:
            nn_fsm_bad_source (blibfabric->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case NN_BLIBFABRIC_STATE_STOPPING_BACKOFF:
        switch (src) {

        case NN_BLIBFABRIC_SRC_RECONNECT_TIMER:
            switch (type) {
            case NN_BACKOFF_STOPPED:
                nn_blibfabric_start_listening (blibfabric);
                return;
            default:
                nn_fsm_bad_action (blibfabric->state, src, type);
            }

        default:
            nn_fsm_bad_source (blibfabric->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (blibfabric->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void nn_blibfabric_start_listening (struct nn_blibfabric *self)
{
    int rc;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    const char *addr;
    const char *end;
    const char *pos;
    uint16_t port;

    /*  First, resolve the IP address. */
    addr = nn_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    nn_assert (pos);
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    nn_assert (rc >= 0);
    port = rc;

    /*  Parse the address. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));
    rc = nn_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    errnum_assert (rc == 0, -rc);

    /*  Combine the port and the address. */
    if (ss.ss_family == AF_INET) {
        ((struct sockaddr_in*) &ss)->sin_port = htons (port);
        sslen = sizeof (struct sockaddr_in);
    }
    else if (ss.ss_family == AF_INET6) {
        ((struct sockaddr_in6*) &ss)->sin6_port = htons (port);
        sslen = sizeof (struct sockaddr_in6);
    }
    else
        nn_assert (0);

    /*  Start listening for incoming connections. */
    rc = nn_usock_start (&self->usock, ss.ss_family, SOCK_STREAM, 0);
    if (nn_slow (rc < 0)) {
        nn_backoff_start (&self->retry);
        self->state = NN_BLIBFABRIC_STATE_WAITING;
        return;
    }

    rc = nn_usock_bind (&self->usock, (struct sockaddr*) &ss, (size_t) sslen);
    if (nn_slow (rc < 0)) {
        nn_usock_stop (&self->usock);
        self->state = NN_BLIBFABRIC_STATE_CLOSING;
        return;
    }

    rc = nn_usock_listen (&self->usock, NN_BLIBFABRIC_BACKLOG);
    if (nn_slow (rc < 0)) {
        nn_usock_stop (&self->usock);
        self->state = NN_BLIBFABRIC_STATE_CLOSING;
        return;
    }
    nn_blibfabric_start_accepting(self);
    self->state = NN_BLIBFABRIC_STATE_ACTIVE;
}

static void nn_blibfabric_start_accepting (struct nn_blibfabric *self)
{
    nn_assert (self->alibfabric == NULL);

    /*  Allocate new alibfabric state machine. */
    self->alibfabric = nn_alloc (sizeof (struct nn_alibfabric), "alibfabric");
    alloc_assert (self->alibfabric);
    nn_alibfabric_init (self->alibfabric, NN_BLIBFABRIC_SRC_ALIBFABRIC, &self->epbase, &self->fsm);

    /*  Start waiting for a new incoming connection. */
    nn_alibfabric_start (self->alibfabric, &self->usock);
}


/***************************NANOMSG_LIBFABRIC FUNCTIONS llazarid***********/

static int run_test()
{
	int ret, i;

	ret = sync_test(false);
	if (ret)
		return ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < opts.iterations; i++) {
		ret = opts.dst_addr ? send_xfer(opts.transfer_size) :
				 recv_xfer(opts.transfer_size, false);
		if (ret)
			return ret;

		ret = opts.dst_addr ? recv_xfer(opts.transfer_size, false) :
				 send_xfer(opts.transfer_size);
		if (ret)
			return ret;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	if (opts.machr)
		show_perf_mr(opts.transfer_size, opts.iterations, &start, &end, 2, opts.argc, opts.argv);
	else
		show_perf(test_name, opts.transfer_size, opts.iterations, &start, &end, 2);

	return 0;
}

static int server_connect_bind(void)
{
	
	struct fi_eq_cm_entry entry;
	uint32_t event;
	struct fi_info *info = NULL;
	ssize_t rd;
	int ret;
	
	printf("\nSERVER");
	rd = fi_eq_sread(eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, eq, "fi_eq_sread", "listen");
		return (int) rd;
	}

	info = entry.info;
	if (event != FI_CONNREQ) {
		fprintf(stderr, "Unexpected CM event %d\n", event);
		ret = -FI_EOTHER;
		goto err;
	}

	ret = fi_domain(fabric, info, &domain, NULL);
	if (ret) {
		FT_PRINTERR("fi_domain", ret);
		goto err;
	}

	ret = ft_alloc_active_res(info);
	if (ret)
		 goto err;

	ret = ft_init_ep();
	if (ret)
		goto err;

	ret = fi_accept(ep, NULL, 0);
	if (ret) {
		FT_PRINTERR("fi_accept", ret);
		goto err;
	}

	rd = fi_eq_sread(eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, eq, "fi_eq_sread", "accept");
		ret = (int) rd;
		goto err;
	}

	if (event != FI_CONNECTED || entry.fid != &ep->fid) {
		fprintf(stderr, "Unexpected CM event %d fid %p (ep %p)\n",
			event, entry.fid, ep);
		ret = -FI_EOTHER;
		goto err;
	}

	fi_freeinfo(info);
	return 0;

err:
	fi_reject(pep, info->handle, NULL, 0);
	fi_freeinfo(info);
	return ret;
}

static int client_connect(void)
{
	
	struct fi_eq_cm_entry entry;
	uint32_t event;
	ssize_t rd;
	int ret;
	
	printf("\nCLIENT 1");
	ret = ft_getsrcaddr(opts.src_addr, opts.src_port, hints);
	printf("\n ret %d", ret);
	if (ret)
		return ret;
	
	
	printf("\nCLIENT 2");
	ret = fi_getinfo(FT_FIVERSION, opts.dst_addr, opts.dst_port, 0, hints, &fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}
	
	ret = ft_open_fabric_res();
	if (ret)
		return ret;

	ret = ft_alloc_active_res(fi);
	if (ret)
		return ret;

	ret = ft_init_ep();
	if (ret)
		return ret;

	ret = fi_connect(ep, fi->dest_addr, NULL, 0);
	if (ret) {
		FT_PRINTERR("fi_connect", ret);
		return ret;
	}

	rd = fi_eq_sread(eq, &event, &entry, sizeof entry, -1, 0);
	if (rd != sizeof entry) {
		FT_PROCESS_EQ_ERR(rd, eq, "fi_eq_sread", "connect");
		ret = (int) rd;
		return ret;
	}

	if (event != FI_CONNECTED || entry.fid != &ep->fid) {
		fprintf(stderr, "Unexpected CM event %d fid %p (ep %p)\n",
			event, entry.fid, ep);
		ret = -FI_EOTHER;
		return ret;
	}
	return 0;
}

static int run(void)
{
	int i, ret = 0;

	if (!opts.dst_addr) {
		printf("\n check1");
		ret = ft_start_server();
		printf("\n check2");
		if (ret)
			return ret;
	}
	
	//ret = opts.dst_addr ? client_connect() : server_connect();
	ret=server_connect_bind();
	if (ret) {
		return ret;
	}

	if (!(opts.options & FT_OPT_SIZE)) {
		for (i = 0; i < TEST_CNT; i++) {
			if (test_size[i].option > opts.size_option)
				continue;
			opts.transfer_size = test_size[i].size;
			init_test(&opts, test_name, sizeof(test_name));
			ret = run_test();
			if (ret)
				goto out;
		}
	} else {
		init_test(&opts, test_name, sizeof(test_name));
		ret = run_test();
		if (ret)
			goto out;
	}

	ret = ft_wait_for_comp(txcq, fi->tx_attr->size - tx_credits);
	/* Finalize before closing ep */
	ft_finalize(fi, ep, txcq, rxcq, FI_ADDR_UNSPEC);
out:
	fi_shutdown(ep, 0);
	return ret;
}
