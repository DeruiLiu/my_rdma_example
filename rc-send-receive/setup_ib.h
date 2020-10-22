#ifndef SETUP_IB_H
#define SETUP_IB_H

#include<infiniband/verbs.h>


struct ibcontext{
    struct ibv_context  *context;
	struct ibv_comp_channel *channel;
    struct ibv_pd       *pd;
    struct ibv_mr       *mr;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_port_attr    port_info;
	int 				pending;
	int rx_depth;

    void                    *buf;
    unsigned long long      size;
};


struct ibdest{
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

//
struct ibcontext *ib_init_ctx(struct ibv_device *ib_dev, unsigned long long size, int rx_depth, int ib_port);
int ib_post_recv(struct ibcontext *ctx, int n);
struct ibdest *ib_server_exch_dest(int port, const struct ibdest *my_dest);
struct ibdest *ib_client_exch_dest(const char *servername, int port, const struct ibdest *my_dest);
void usage(const char *argv0);
int connect_between_qps(struct ibcontext *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl,struct ibdest *dest, int sgid_idx);
int post_send(struct ibcontext * ctx);
//
int ib_close_ctx(struct ibcontext *ctx);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

#endif
