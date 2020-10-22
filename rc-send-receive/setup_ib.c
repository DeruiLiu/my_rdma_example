#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>

#include "setup_ib.h"


enum ibv_mtu ib_mtu_to_enum(int mtu)
{
    switch (mtu) {
        case 256:
            return IBV_MTU_256;
        case 512:
            return IBV_MTU_512;
        case 1024:
            return IBV_MTU_1024;
        case 2048:
            return IBV_MTU_2048;
        case 4096:
            return IBV_MTU_4096;
        default:
            return -1;
    }
}

enum {
    IB_RECV_WRID = 1,
    IB_SEND_WRID = 2,
};

void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port=<port>         listen on/connect to port <port> (default 18515)\n");
    printf("  -d, --ib-dev=<dev>        use IB device <dev> (default first device found)\n");
    printf("  -i, --ib-port=<port>      use port <port> of IB device (default 1)\n");
    printf("  -s, --size=<size>         size of message to exchange (default 4096)\n");
    printf("  -m, --mtu=<size>          path MTU (default 1024)\n");
    printf("  -r, --rx-depth=<dep>      number of receives to post at a time (default 500)\n");
    printf("  -n, --iters=<iters>       number of exchanges (default 1000)\n");
    printf("  -l, --sl=<sl>             service level value\n");
    printf("  -e, --events              sleep on CQ events (default poll)\n");
    printf("  -g, --gid-idx=<gid index> local port gid index\n");
    printf("  -c, --contiguous-mr       use contiguous mr\n");
    printf("  -t, --inline-recv=<size>  size of inline-recv\n");
    printf("  -a, --check-nop	    check NOP opcode\n");
    printf("  -o, --odp		    use on demand paging\n");
    printf("  -u, --upstream            use upstream API\n");
    printf("  -t, --upstream            use upstream API\n");
    printf("  -z, --contig_addr         use specifix addr for contig pages MR, must use with -c flag\n");
    printf("  -b, --ooo                 enable multipath processing\n");
    printf("  -j, --memic         	    use device memory\n");
}


struct ibcontext *ib_init_ctx(struct ibv_device *ib_dev, unsigned long long size, int rx_depth, int ib_port)
{
    struct ibcontext *ctx;
    struct ibv_device_attr dattr;
    int ret;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    memset(&dattr, 0, sizeof(dattr));

    ctx->size = size;
    ctx->rx_depth = rx_depth;
	ctx->buf = memalign(sysconf(_SC_PAGESIZE),size);
	if(!ctx->buf){
		fprintf(stderr,"Couldn't allocate work buf.\n");
		goto clean_ctx;
	}

    ctx->context = ibv_open_device(ib_dev); // 根据设备名打开设备，返回IB上下文
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
        goto clean_buffer;
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        goto clean_pd;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        goto clean_mr;
    }

    ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        goto clean_cq;
    }

    {
        struct ibv_qp_init_attr attr = {
			.send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .cap	 = {
                .max_send_wr = 1,
                .max_recv_wr = rx_depth,
                .max_send_sge = 1,
                .max_recv_sge = 1
			},
            .qp_type = IBV_QPT_RC, //以RC方式通信
		};

        ctx->qp = ibv_create_qp(ctx->pd, &attr);
        if (!ctx->qp) {
            fprintf(stderr, "Couldn't create QP\n");
            goto clean_qp;
        }
    }

    {
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = ib_port,
            .qp_access_flags = 0
        };

        if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

    return ctx;

clean_qp:
    ibv_destroy_qp(ctx->qp);

clean_cq:
    ibv_destroy_cq(ctx->cq);

clean_mr:
    ibv_dereg_mr(ctx->mr);

clean_comp_channel:
    if (ctx->channel)
        ibv_destroy_comp_channel(ctx->channel);

clean_pd:
    ibv_dealloc_pd(ctx->pd);

clean_device:
    ibv_close_device(ctx->context);

clean_buffer:
    free(ctx->buf);

clean_ctx:
    free(ctx);

    return NULL;
}
#define mmin(a, b) a < b ? a : b
#define MAX_SGE_LEN 0xFFFFFFF
int ib_post_recv(struct ibcontext *ctx, int n)
{
    struct ibv_sge list = {
        .addr = (uintptr_t)ctx->buf,
        .length = mmin(ctx->size, MAX_SGE_LEN),
        .lkey = ctx->mr->lkey
    };

    struct ibv_recv_wr wr = {
        .wr_id = IB_RECV_WRID,
        .sg_list = &list,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_wr;
    int i;
    for (i = 0; i < n; i++) {
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;
    }
    return i;
}
struct ibdest *ib_server_exch_dest(int port, const struct ibdest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct ibdest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d \n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d \n", port);
        return NULL;
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);

    if (connfd < 0) {
        fprintf(stderr, "accept() afiled \n");
        return NULL;
    }

    n = recv(connfd, msg, sizeof(msg), MSG_WAITALL);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: couldn't read remote address \n", n, (int)sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    // 将message中的数据写入到指定字符串中
    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    gid_to_wire_gid(&my_dest->gid, gid);

    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(connfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address \n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    /* expecting "done" msg */
    if (read(connfd, msg, sizeof(msg)) <= 0) {
        fprintf(stderr, "Couldn't read \"done\" msg\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }


out:
    close(connfd);
    return rem_dest;
}
struct ibdest *ib_client_exch_dest(const char *servername, int port, const struct ibdest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,    // allow ipv4 or ipv6
        .ai_socktype = SOCK_STREAM // TCP
    };

    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct ibdest *rem_dest = NULL;
    char gid[33];

    // 把格式化的数据写入某个字符串缓冲区，在不确定字符串的长度时，非常灵活方便，能够根据格式化的字符串长度，申请足够的内存空间
    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    // getaddrinfo()函数能够处理名字到地址以及服务端口这两种转换
    n = getaddrinfo(servername, service, &hints, &res);

    // gai_strerror()和getaddrinfo()匹配的报错函数
    if (n < 0) {
        fprintf(stderr, "%s for %s:%d \n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s : %d \n", servername, port);
        return NULL;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid); // 将格式化的数据写入msg字符串中

    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (recv(sockfd, msg, sizeof msg, MSG_WAITALL) != sizeof msg) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address \n");
        goto out;
    }

    if (write(sockfd, "done", sizeof("done")) != sizeof("done")) {
        fprintf(stderr, "Couldn't send \"done\" msg \n");
        goto out;
    }

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

out:
    close(sockfd);
    return rem_dest;
}

int connect_between_qps(struct ibcontext *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct ibdest *dest,
    int sgid_idx)
{
    struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_RTR,
        .path_mtu = mtu,
        .dest_qp_num = dest ->qpn,
        .rq_psn = dest -> psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer		= 12,
        .ah_attr		={
            .is_global 	= 0,
            .dlid		=dest->lid,
            .sl			=sl,
            .src_path_bits=0,
            .port_num	=port
		}
	};

    enum ibv_qp_attr_mask attr_mask;

    attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(ctx->qp, &attr, attr_mask)) {
        fprintf(stderr, "Failed to modify QP to RTR \n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
        IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    return 0;
}

int post_send(struct ibcontext *ctx)
{
    struct ibv_sge list = {
        .addr = (uintptr_t)ctx->buf,
        .length = mmin(ctx->size, MAX_SGE_LEN),
        .lkey = ctx->mr->lkey
    };
    struct ibv_send_wr wr = {
        .wr_id = IB_SEND_WRID,
        .sg_list = &list,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int ib_close_ctx(struct ibcontext *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx);

    return 0;
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    uint32_t v32;
    uint32_t *raw = (uint32_t *)gid->raw;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        raw[i] = ntohl(v32);
    }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    int i;
    uint32_t *raw = (uint32_t *)gid->raw;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(raw[i]));
}
