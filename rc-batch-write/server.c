/*
use basic api to establish a basic connect
basic client-server with send/receive
*/
#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#define _GNU_SOURCE

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
#include <sys/time.h>

#include "setup_ib.h"
static int page_size;
static void *contig_addr;

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct ibcontext *ctx;
    struct ibdest my_dest;
    struct ibdest *rem_dest; // 即表示对端的ib信息
    char *ib_devname;
    char *servername;
    int port = 18515;
    int ib_port = 1;
    unsigned long long size = 4096;
    enum ibv_mtu mtu = IBV_MTU_1024;
    int rx_depth = 1000; // 接收队列的最大深度
    int iters = 1000;
    int routs;
    int rcnt, scnt;
    int sl = 0;
    int gidx = -1;
    char gid[INET6_ADDRSTRLEN];
    struct timeval start, end;

    while (1) {
        int c;
        static struct option long_options[] = {
            { .name = "port",          .has_arg = 1, .val = 'p' },
            { .name = "ib-dev",        .has_arg = 1, .val = 'd' },
            { .name = "ib-port",       .has_arg = 1, .val = 'i' },
            { .name = "size",          .has_arg = 1, .val = 's' },
            { .name = "mtu",           .has_arg = 1, .val = 'm' },
            { .name = "rx-depth",      .has_arg = 1, .val = 'r' },
            { .name = "iters",         .has_arg = 1, .val = 'n' },
            { .name = "sl",            .has_arg = 1, .val = 'l' },
            { .name = "gid-idx",       .has_arg = 1, .val = 'g' },
            { 0 }
        };

        c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:g:", long_options, NULL);
        if (c == -1)
            break;

        switch (c) {
            case 'p':
                port = strtol(optarg, NULL, 0);
                if (port < 0 || port > 65535) {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 'd':
                ib_devname = strdupa(optarg);
                break;

            case 'i':
                ib_port = strtol(optarg, NULL, 0);
                if (ib_port < 0)
                    ;
                {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 's':
                size = strtoll(optarg, NULL, 0);
                break;

            case 'm':
                mtu = ib_mtu_to_enum(strtol(optarg, NULL, 0));
                if (mtu < 0) {
                    usage(argv[0]);
                    return 1;
                }
                break;

            case 'r':
                rx_depth = strtol(optarg, NULL, 0);
                break;

            case 'n':
                iters = strtol(optarg, NULL, 0);
                break;

            case 'l':
                sl = strtol(optarg, NULL, 0);
                break;

            case 'g':
                gidx = strtol(optarg, NULL, 0);
                break;

            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind == argc - 1)
        servername = strdupa(argv[optind]);
    else if (optind < argc) {
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE); // 系统存储页的长度

    dev_list = ibv_get_device_list(NULL); // 返回系统中可用的设备列表
    if (!dev_list) {
        perror("Failed to get IB device list");
        return 1;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i) {
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        }
        ib_dev = dev_list[i];
        if (!ib_dev) {
            fprintf(stderr, "IB device %s not found \n", ib_devname);
            return 1;
        }
        
    }


    ctx = ib_init_ctx(ib_dev, size, rx_depth, ib_port);
    if (!ctx)
        return 1;

    // 该api查询的是IB端口的信息
    if (ibv_query_port(ctx->context, ib_port, &ctx->port_info)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_dest.lid = ctx->port_info.lid; // 由于子网管理器分配给终端节点的16位地址，每个LID在其所在的子网中都是唯一的
    if (ctx->port_info.link_layer != IBV_LINK_LAYER_ETHERNET && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID \n");
        return 1;
    }

    if (gidx >= 0) {
        if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
            fprintf(stderr, "can't read sgid of index %d \n", gidx);
            return 1;
        }
    } else {
        memset(&my_dest.gid, 0, sizeof my_dest.gid);
    }

    // get QPN
    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    // n表示数值，p表示表达
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
    printf("local address : LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s \n", my_dest.lid, my_dest.qpn, my_dest.psn, gid);

    // rem_dest=ib_client_exch_dest(servername,port,&my_dest);//作为client,交换双方的QP信息
    rem_dest = ib_server_exch_dest(port, &my_dest);
    if (!rem_dest)
        return 1;

    // 将数值格式转化为点分十进制的IP地址格式
    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
    printf("remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n", rem_dest->lid, rem_dest->qpn,
        rem_dest->psn, gid);

    // 将QP调整至RTS状态
    if (connect_between_qps(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        return 1;
    }

    struct ib_remote_mr remote_mr;

    if(info_post_recv(ctx,sizeof(struct ib_remote_mr))){
        fprintf(stderr,"Could n't post info recv\n");
        return 1;
    }
    wait_completions(ctx,IB_RECV_WRID);
    memcpy(&remote_mr,ctx->buf,sizeof(struct ib_remote_mr));
    ctx->rkey = remote_mr.rkey;
    ctx->raddr = remote_mr.raddr;

    remote_mr.raddr = (uint64_t)ctx->buf;
    remote_mr.rkey = ctx->mr->rkey;
    memcpy(ctx->buf,&remote_mr,sizeof(struct ib_remote_mr));
    if(info_post_send(ctx,IB_SEND_WRID,sizeof(struct ib_remote_mr))){
        fprintf(stderr,"failed to send my mr info\n");
        return 1;
    }
    wait_completions(ctx,IB_SEND_WRID);
    
    // pingpong需要互相发送消息，即先通过调用ib_post_recv将WQE塞满接收队列
    // 如果只是client往server发送消息应该可以不需要此步骤
    routs = ib_post_recv(ctx, ctx->rx_depth);
    if (routs < ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", routs);
        return 1;
    }
    if (gettimeofday(&start, NULL)) { // 获取开始的时间
        perror("gettimeofday");
        return 1;
    }

    // 正式开始发送数据
    rcnt = scnt = 0;
    while (rcnt < iters) {
        struct ibv_wc wc[2];
        int ne, i;
        do {
            ne = ibv_poll_cq(ctx->cq, 2, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d \n", ne);
                return 1;
            }
        } while (ne < 1); // 因为发送队列为1，所以每次只能塞1个wr到发送队列中

        for (int i = 0; i < ne; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d \n", ibv_wc_status_str(wc[i].status), wc[i].status,
                    (int)wc[i].wr_id);
                return 1;
            }
            if ((int)wc[i].wr_id == IB_RECV_WRID) {
                if (--routs <= 1) {
                    routs += ib_post_recv(ctx, ctx->rx_depth - routs);
                    if (routs < ctx->rx_depth) {
                        fprintf(stderr, "Couldn't post receive (%d) \n", routs);
                        return 1;
                    }
                }
                ++rcnt;
            } else {
                fprintf(stderr, "Completion for unknown wr_id %d\n", (int)wc[i].wr_id);
                return 1;
            }
        }
    }

    if (gettimeofday(&end, NULL)) {
        perror("gettimeofday");
        return 1;
    }

    {
        float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        long long bytes = (long long)size * iters;
        float time_gap = end.tv_usec - start.tv_usec;
        printf("timegap is %f \n", usec);
        printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n", bytes, usec / 1000000., bytes * 8. / usec);
        printf("%d iters in %.2f seconds = %.2f usec/iter\n", iters, usec / 1000000., usec / iters);
    }

    if (ib_close_ctx(ctx))
        return 1;

    ibv_free_device_list(dev_list);
    free(rem_dest);
    return 0;
}
