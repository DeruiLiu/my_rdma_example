#ifndef SETUP_IB_H
#define SETUP_IB_H

#include <infiniband/verbs.h>

enum {
    IB_RECV_WRID = 1,
    IB_SEND_WRID = 2,
    IB_WRITE_WRID = 4,
};


struct ibcontext {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_info;
    int pending;
    int rx_depth;

    void *buf;
    unsigned long long size;

    int rkey;//remote key
    uint64_t raddr;//remote address
};

struct ibdest {
    int lid;//LID of the IB port 
    int qpn;//qp number
    int psn;//packet series number
    union ibv_gid gid;
};

struct ib_Remote_mr{
    uint64_t raddr;
    uint32_t rkey;
};

//
struct ibcontext *ib_init_ctx(struct ibv_device *ib_dev, unsigned long long size, int rx_depth, int ib_port);
int ib_post_recv(struct ibcontext *ctx, int n);
struct ibdest *ib_server_exch_dest(int port, const struct ibdest *my_dest);
struct ibdest *ib_client_exch_dest(const char *servername, int port, const struct ibdest *my_dest);
void usage(const char *argv0);
int connect_between_qps(struct ibcontext *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct ibdest *dest,
    int sgid_idx);
int post_send(struct ibcontext *ctx);
//
int info_post_send(struct ibcontext *ctx,int info_size);
int info_post_recv(struct ibcontext *ctx,int size);
int wait_completions(struct ibcontext *ctx,int wr_id);
int ib_close_ctx(struct ibcontext *ctx);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
enum ibv_mtu ib_mtu_to_enum(int mtu);

#endif
