/*
 * GLINK over SMEM with the ADSP (the "lpass" edge), polled, several channels:
 * Linux qcom_glink_native.c + qcom_glink_smem.c, as userland/wland/glink.c +
 * link.c do for the modem (one channel there). The remote opens a channel;
 * we ack it and open ours if the name is in our table.
 */
#ifndef EDGE_H
#define EDGE_H
#include <stddef.h>
#include <stdint.h>

typedef void (*edge_rx_t)(const uint8_t *data, uint32_t len);

struct edge_chan {
	const char *name;
	edge_rx_t rx;
	/* state */
	uint16_t lcid;
	uint32_t rcid;
	int remote_open, local_open, acked;
};

extern int edge_verbose;
uint64_t edge_ms(void);			/* since the first call */
int edge_init(unsigned host, unsigned ipc_bit, struct edge_chan *chans, unsigned n);
int edge_poll(unsigned ms);		/* handle what arrived, for ms (0 = once) */
int edge_ready(const struct edge_chan *c);	/* both ends open and acked */
int edge_send(struct edge_chan *c, const void *data, uint32_t len);
/* hand the edge to a new process: save the state of a running edge; adopt it (no FIFO reset, no handshake) */
int edge_save(const char *path);
int edge_adopt(const char *path, unsigned host, unsigned ipc_bit, struct edge_chan *chans, unsigned n);
/* open our end of a channel the remote opened earlier (its rcid from the log), e.g. after an adoption */
int edge_open_late(struct edge_chan *c, uint32_t rcid);

#endif
