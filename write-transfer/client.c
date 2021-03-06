#include <fcntl.h>
#include <libgen.h>

#include "common.h"
#include "messages.h"

struct client_context{
    char *buffer;
    struct ibv_mr *buffer_mr;

    struct message *msg;
    struct ibv_mr *msg_mr;

    uint64_t peer_addr;
    uint32_t peer_rkey;

    int fd;//文件描述符
    const char* file_name;
};

static void post_receive(struct rdma_cm_id * id){
    struct client_context *ctx = (struct client_context *)id->context;
    struct ibv_recv_wr wr,*bad_wr = NULL;
    struct ibv_sge sge;
    memset(&wr,0,sizeof(wr));

    sge.addr = (uintptr_t)ctx->msg;
    sge.length = sizeof(*ctx->msg);
    sge.lkey = ctx->msg_mr->lkey;

    wr.wr_id = (uintptr_t)id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    TEST_NZ(ibv_post_recv(id->qp,&wr,&bad_wr));
}

static void on_pre_conn(struct rdma_cm_id *id){
    struct client_context *ctx = (struct client_context *)id->context;//id->context是一个空指针

    posix_memalign((void **)&ctx->buffer,sysconf(_SC_PAGESIZE),BUFFER_SIZE);//发送端的ctx->buffer用来发送数据
    TEST_Z(ctx->buffer_mr = ibv_reg_mr(rc_get_pd(),ctx->buffer,BUFFER_SIZE,0));

    posix_memalign((void **)&ctx->msg,sysconf(_SC_PAGESIZE),sizeof(*ctx->msg));//发送端的ctx->msg是用来接收数据
    TEST_Z(ctx->msg_mr = ibv_reg_mr(rc_get_pd(),ctx->msg,sizeof(*ctx->msg),IBV_ACCESS_LOCAL_WRITE));

    post_receive(id);
}

static void write_remote(struct rdma_cm_id *id,uint32_t len){
    struct client_context* ctx=(struct client_context*)id->context;

    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;
    memset(&wr,0,sizeof(wr));

    wr.wr_id = (uintptr_t)id;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(len);
    wr.wr.rdma.remote_addr = ctx->peer_addr;
    wr.wr.rdma.rkey = ctx->peer_rkey;

    if(len){
        wr.sg_list = &sge;
        wr.num_sge = 1;
        sge.addr = (uintptr_t)ctx->buffer;//本地的数据,用来发送的数据的地址
        sge.length = len;
        sge.lkey = ctx->buffer_mr->lkey;
    }

    TEST_NZ(ibv_post_send(id->qp,&wr,&bad_wr));
}
static void send_file_name(struct rdma_cm_id *id){
    struct client_context *ctx = (struct client_context *)id->context;
    strcpy(ctx->buffer,ctx->file_name);//将file_name复制到ctx->buffer

    write_remote(id,strlen(ctx->file_name)+1);
}

static void send_next_chunk(struct rdma_cm_id *id){
    struct client_context *ctx = (struct client_context *)id->context;

    ssize_t size = 0;
    size = read(ctx->fd,ctx->buffer,BUFFER_SIZE);//从fd中往ctx->buffer读取buffer_size大小的数据

    if(size == -1){
        rc_die("read() failed \n");
    }

    write_remote(id,size);
}
static void on_completion(struct ibv_wc *wc){
    //server先给client发送一个地址，然后client再往这个地址上去写数据
    struct rdma_cm_id *id = (struct rdma_cm_id *)(uintptr_t)(wc->wr_id);
    struct client_context *ctx = (struct client_context *)id->context;

    if(wc->opcode & IBV_WC_RECV){
        if(ctx->msg->id == MSG_MR){
            ctx->peer_addr = ctx->msg->data.mr.addr;
            ctx->peer_rkey = ctx->msg->data.mr.rkey;

            printf("received MR, sending file name\n");
            send_file_name(id);
            post_receive(id);

        }else if(ctx->msg->id == MSG_READY){
            printf("received READY, sending chunk\n");
            send_next_chunk(id);
            post_receive(id);
        }else if(ctx->msg->id == MSG_DONE){
            printf("received DONE, disconnecting \n");
            rc_disconnect(id);
        }
        
    }
}

int main(int argc, char **argv)
{
    struct client_context ctx;
    if(argc != 3){
        fprintf(stderr,"usage: %s <server-address> <file-name>\n",argv[0]);
        return 1;
    }

    ctx.file_name = basename(argv[2]);
    ctx.fd = open(argv[2],O_RDONLY);

    if (ctx.fd == -1) {
        fprintf(stderr,"unable to open input file \"%s\"\n",ctx.file_name);
        return 1;
    }
    //通过函数指针调用，可使得client和server端同样的函数指针绑定不同的函数，从而使得common.c中的逻辑完全一样
    rc_init(
        on_pre_conn,
        NULL,// on connect
        on_completion,
        NULL//on disconnect
    );
    rc_client_loop(argv[1],DEFAULT_PORT,&ctx);
    close(ctx.fd);//关闭文件

    return 0;
}
