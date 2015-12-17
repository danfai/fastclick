// -*- c-basic-offset: 4; related-file-name: "../include/click/netmapdevice.hh" -*-
/*
 * netmapinfo.{cc,hh} -- library for interfacing with netmap
 * Eddie Kohler, Luigi Rizzo, Tom Barbette
 *
 * Copyright (c) 2012 Eddie Kohler
 * Copyright (c) 2015 University of Liege
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/netmapdevice.hh>

/****************************
 * NetmapBufQ
 ****************************/
NetmapBufQ::NetmapBufQ() : _head(0),_count(0){

}

NetmapBufQ::~NetmapBufQ() {

}

/**
 * Initialize the per-thread pools and the cleanup pool
 */
int NetmapBufQ::static_initialize(struct nm_desc* nmd) {
	if (!nmd) {
		click_chatter("Error:Null netmap descriptor in NetmapBufQ::static_initialize!");
		return 1;
	}

	//Only initilize once
	if (NetmapDevice::some_nmd)
		return 0;

	NetmapDevice::some_nmd = nmd;

	buf_size = nmd->some_ring->nr_buf_size;
	buf_start = reinterpret_cast<unsigned char *>(nmd->buf_start);
	buf_end = reinterpret_cast<unsigned char *>(nmd->buf_end);
	max_index = (buf_end - buf_start) / buf_size;

	if (!netmap_buf_pools) {
		netmap_buf_pools = new NetmapBufQ*[click_max_cpu_ids()];

		for (unsigned i = 0; i < click_max_cpu_ids(); i++) {
			netmap_buf_pools[i] = new NetmapBufQ();
		}
	}

	if (nmd->req.nr_arg3 > 0) {
		NetmapDevice::global_alloc -= nmd->req.nr_arg3;
		click_chatter("Allocated %d buffers from Netmap buffer pool",nmd->req.nr_arg3);
		NetmapBufQ::global_insert_all(nmd->nifp->ni_bufs_head,nmd->req.nr_arg3);
		nmd->nifp->ni_bufs_head = 0;
		nmd->req.nr_arg3 = 0;
	}

	return 0;
}

/**
 * Empty all NetmapBufQ and the global ring. Return all netmap buffers in a list
 */
uint32_t NetmapBufQ::static_cleanup()
{
	if (!netmap_buf_pools)
		return 0;

	for (unsigned int i = 1; i < click_max_cpu_ids(); i++) {
		if (netmap_buf_pools[i]) {
			if (netmap_buf_pools[i]->_head)
				netmap_buf_pools[0]->insert_all(netmap_buf_pools[i]->_head,false);
			delete netmap_buf_pools[i];
			netmap_buf_pools[i] = NULL;
		}
	}

	while (global_buffer_list > 0) {
		uint32_t idx=global_buffer_list;
		global_buffer_list = BUFFER_NEXT_LIST(global_buffer_list);
		netmap_buf_pools[0]->insert_all(idx, false);
	}

	uint32_t idx = 0;
	if (netmap_buf_pools[0]->_count > 0) {
		if (netmap_buf_pools[0]->_count == netmap_buf_pools[0]->count_buffers(idx))
			click_chatter("Error on cleanup of netmap buffer ! Expected %d buffer, got %d",netmap_buf_pools[0]->_count,netmap_buf_pools[0]->count_buffers(idx));
		else
			click_chatter("Freeing %d Netmap buffers",netmap_buf_pools[0]->_count);
		idx = netmap_buf_pools[0]->_head;
		netmap_buf_pools[0]->_head = 0;
		netmap_buf_pools[0]->_count = 0;
	}
	delete netmap_buf_pools[0];
	netmap_buf_pools[0] = 0;
	delete[] netmap_buf_pools;
	netmap_buf_pools = 0;
	return idx;
}

/**
 * Insert all netmap buffers inside the global list
 */
void NetmapBufQ::global_insert_all(uint32_t idx, int count) {
	//Cut packets in global pools
	while (count >= NETMAP_PACKET_POOL_SIZE) {
		int c = 0;
		BUFFER_NEXT_LIST(idx) = global_buffer_list;
		global_buffer_list = idx;
		uint32_t *p = 0;
		while (c < NETMAP_PACKET_POOL_SIZE) {
			p = BUFFER_PTR(idx);
			idx = *p;
			c++;
		}
		*p = 0;
		count -= NETMAP_PACKET_POOL_SIZE;
	}

	//Add remaining buffer to the local pool
	if (count > 0) {
		NetmapBufQ::local_pool()->insert_all(idx, true);
	}
}

/***************************
 * NetmapDevice
 ***************************/

NetmapDevice::NetmapDevice(String ifname) : ifname(ifname),n_queues(0),_minfd(INT_MAX),_maxfd(INT_MIN),_use_count(0)  {
	n_refs = 0;
}

NetmapDevice::~NetmapDevice() {
	nics.remove(ifname);

    //Replace buffer with NS_NOFREE inside rings as they will be freed by the NetmapBufQ cleaner
    for (int i = 0; i < n_queues; i++) {
        struct nm_desc* nmd = nmds[i];
        struct netmap_ring* txring = NETMAP_TXRING(nmd->nifp,i);
        for (unsigned j = 0; j < txring->num_slots; j++) {
            struct netmap_slot* slot = &txring->slot[j];
            if (slot->flags & NS_NOFREE) {
                slot->buf_idx = NetmapBufQ::local_pool()->extract();
                slot->flags &= ~NS_NOFREE;
            }
        }
    }

	click_chatter("Detaching netmap device %s",ifname.c_str());
	if (parent_nmd) {
		for (int i = 0; i < n_queues; i++) {
			if (NetmapDevice::some_nmd != nmds[i]) {
				nm_close(nmds[i]);
			}
			nmds[i] = NULL;
		}
		parent_nmd = NULL;
	};
}

NetmapDevice* NetmapDevice::open(String ifname) {
	NetmapDevice* d = nics.find(ifname);
	if (d == NULL) {
		d = new NetmapDevice(ifname);
		if (d->initialize() != 0) {
			return NULL;
		}
		nics.insert(ifname,d);
	}

	d->_use_count++;
	return d;
}

int NetmapDevice::initialize() {
	struct nm_desc* g_nmd;


	g_nmd = nm_open(ifname.c_str(), NULL, 0, NULL);

	if (!g_nmd) {
		return -1;
	}

	if (g_nmd == NULL) {
		click_chatter("Unable to open %s: %s", ifname.c_str(), strerror(errno));
		return 1;
	}

	n_queues = g_nmd->nifp->ni_rx_rings;
	nmds.resize(n_queues);
	for (int i = 0; i < n_queues; i++) {
		struct nm_desc child_nmd = *g_nmd; //Copy mem, arg2, ...
		child_nmd.self = &child_nmd;

		int flags = NM_OPEN_IFNAME;

		if (some_nmd) {
			child_nmd.mem = some_nmd->mem;
			child_nmd.memsize = some_nmd->memsize;
			flags |= NM_OPEN_NO_MMAP;
		} else {
			child_nmd.mem = NULL;
			child_nmd.req.nr_arg3 = NetmapDevice::global_alloc;
			flags |= NM_OPEN_ARG3;
			if (child_nmd.req.nr_arg3 % NETMAP_PACKET_POOL_SIZE != 0)
				child_nmd.req.nr_arg3 = ((child_nmd.req.nr_arg3) + 1) * NETMAP_PACKET_POOL_SIZE;
	#if HAVE_ZEROCOPY
			//Ensure we have at least a batch per thread + 1
			if (NETMAP_PACKET_POOL_SIZE * ((unsigned)click_nthreads + 1) > child_nmd.req.nr_arg3)
				child_nmd.req.nr_arg3 = NETMAP_PACKET_POOL_SIZE * (click_nthreads + 1);
	#endif
		}

		if (i == 0) {
			nm_close(g_nmd);
		} else {

		}
		child_nmd.req.nr_flags = NR_REG_ONE_NIC;
		child_nmd.req.nr_ringid =  i | NETMAP_NO_TX_POLL;

		struct nm_desc* thread_nm = nm_open(ifname.c_str(), NULL, flags,  &child_nmd);

		if (thread_nm == NULL) {
			return -1;
		}

		if (thread_nm->fd < _minfd)
			_minfd = thread_nm->fd;
		if (thread_nm->fd > _maxfd)
			_maxfd = thread_nm->fd;

		nmds[i] = thread_nm;

		if (i == 0) {
			if (NetmapDevice::some_nmd == 0) {
				//Allocate packet pools if not already done
				if (NetmapBufQ::static_initialize(thread_nm) != 0) {
					nm_close(thread_nm);
					return -1;
				}
			}

			parent_nmd = thread_nm;
			if (parent_nmd->nifp->ni_name[0] == '\0')
				strcpy(parent_nmd->nifp->ni_name,&(ifname.c_str()[7]));
		}
	}

	return 0;
}

void NetmapDevice::destroy() {
		--_use_count;
		if (_use_count == 0)
			delete this;
}

void NetmapDevice::static_cleanup() {
	uint32_t idx = NetmapBufQ::static_cleanup();
	if (idx != 0) {
		if (some_nmd) {
			some_nmd->nifp->ni_bufs_head = idx;
			nm_close(some_nmd);
			some_nmd = 0;
		} else {
			click_chatter("No NMD set and netmap packet not released !");
		}
	}
}

NetmapBufQ** NetmapBufQ::netmap_buf_pools = 0;
unsigned int NetmapBufQ::buf_size = 0;
unsigned char* NetmapBufQ::buf_start = 0;
unsigned char* NetmapBufQ::buf_end = 0;
uint32_t NetmapBufQ::max_index = 0;

Spinlock NetmapBufQ::global_buffer_lock;
uint32_t NetmapBufQ::global_buffer_list = 0;

int NetmapBufQ::messagelimit = 0;
HashMap<String,NetmapDevice*> NetmapDevice::nics;
struct nm_desc* NetmapDevice::some_nmd = 0;

int NetmapDevice::global_alloc = 0;

CLICK_ENDDECLS
