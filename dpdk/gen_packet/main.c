/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
/* dpdk */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_timer.h>
/* librte_net */
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
/* dump packet contents */
#include <rte_hexdump.h>
#include <rte_malloc.h>
/* logging */
#include <rte_log.h>
/* atomic counter */
#include <rte_atomic.h>
#include "paxos.h"
#include "rte_paxos.h"
#include "const.h"

static uint32_t cur_inst;

static const struct rte_eth_conf port_conf_default = {
        .rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

static struct rte_timer timer;

static rte_atomic32_t counter = RTE_ATOMIC32_INIT(0);

static void
generate_packets(struct rte_mbuf **bufs, unsigned nb_tx)
{
    char str[] = "Hello";
    struct paxos_accepted accepted = {
        .iid = 1,
        .ballot = 1,
        .value_ballot = 1,
        .aid = 0,
        .value = {sizeof(str), str},
    };
    struct paxos_message pm;
    pm.type = (PAXOS_ACCEPTED);
    pm.u.accepted = accepted;
	unsigned i;
	for (i = 0; i < nb_tx; i++) {
	    add_paxos_message(&pm, bufs[i], 12345, LEARNER_PORT);
        pm.u.accepted.iid = cur_inst;
        rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER8,
            "submit instance %u\n", cur_inst);
    }
	send_batch(bufs, nb_tx, 0);
}

static int
send_initial_packets(void *arg)
{
    int ret;
    struct rte_mempool *mbuf_pool = (struct rte_mempool*) arg;
    struct rte_mbuf *bufs[TX_RING_SIZE];
    ret = rte_pktmbuf_alloc_bulk(mbuf_pool, bufs, TX_RING_SIZE);
    while(1)
        generate_packets(bufs, TX_RING_SIZE);
    return ret;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = 1, tx_rings = 1;
    int retval;
    uint16_t q;
    if (port >= rte_eth_dev_count())
            return -1;
    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
            return retval;
    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
            retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
                            rte_eth_dev_socket_id(port), NULL, mbuf_pool);
            if (retval < 0)
                    return retval;
    }
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++) {
            retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
                            rte_eth_dev_socket_id(port), NULL);
            if (retval < 0)
                    return retval;
    }
    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0)
            return retval;
    /* Enable RX in promiscuous mode for the Ethernet device. */
    rte_eth_promiscuous_enable(port);
    return 0;
}
/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static void
lcore_main(uint8_t port)
{
    /*
     * Check that the port is on the same NUMA node as the polling thread
     * for best performance.
     */
    if (rte_eth_dev_socket_id(port) > 0 &&
                rte_eth_dev_socket_id(port) !=
                (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to "
                    "polling thread.\n\tPerformance will "
                    "not be optimal.\n", port);

    printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n", rte_lcore_id());
    /* Run until the application is quit or killed. */
    for (;;) {
        if (force_quit)
            break;
        struct rte_mbuf *bufs[BURST_SIZE];
        uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
        rte_atomic32_add(&counter, nb_rx);
         if (unlikely(nb_rx == 0))
                continue;
        rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_TIMER, "Received %8d packets\n", nb_rx);
        /* Free packets. */
        uint16_t idx;
        for (idx = 0; idx < nb_rx; idx++)
            rte_pktmbuf_free(bufs[idx]);
    }
}

static void
report_stat(struct rte_timer *tim, __attribute((unused)) void *arg)
{
    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_TIMER,
            "%s on core %d\n", __func__, rte_lcore_id());

    int count = rte_atomic32_read(&counter);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_TIMER, "%8d\n", count);
    rte_atomic32_set(&counter, 0);
	if (force_quit)
		rte_timer_stop(tim);
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER8,
		"\n\nSignal %d received, preparing to exit...\n", signum);
		force_quit = true;
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
    struct rte_mempool *mbuf_pool;
    unsigned master_core, lcore_id;
    uint8_t portid = 0;
	force_quit = false;
    /* Learner's instance starts at 1 */
    cur_inst = 1;
    /* Initialize the Environment Abstraction Layer (EAL). */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	rte_timer_init(&timer);
    uint64_t hz = rte_get_timer_hz();
    /* increase call frequency to rte_timer_manage() to increase the precision */
    TIMER_RESOLUTION_CYCLES = hz / 100;
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_USER8,
            "1 cycle is %3.2f ns\n", 1E9 / (double)hz);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

    /* Creates a new mempool in memory to hold the mbufs. */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
            MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
            rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    /* Initialize port 0. */
    if (port_init(portid, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", portid);

    /* master core */
    master_core = rte_lcore_id();
    /* slave core */
    lcore_id = rte_get_next_lcore(master_core, 0, 1);
	rte_timer_reset(&timer, hz, PERIODICAL, lcore_id, report_stat, NULL);
	rte_timer_subsystem_init();
	rte_eal_remote_launch(check_timer_expiration, NULL, lcore_id);

    /* slave core */
    lcore_id = rte_get_next_lcore(lcore_id, 0, 1);
	rte_eal_remote_launch(send_initial_packets, mbuf_pool, lcore_id);

    /* Call lcore_main on the master core only. */
    lcore_main(portid);
    return 0;
}