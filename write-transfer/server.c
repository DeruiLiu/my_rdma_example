#include <fcntl.h>
#include <sys/stat.h>

#include "common.h"
#include "messages.h"

#define MAX_FILE_NAME 256

struct conn_context{
    char *buffer;
    struct ibv_mr *buffer_mr;

    struct message *msg;
    struct ibv_mr *msg_mr;

    int fd;
    char file_name[MAX_FILE_NAME];
};

static void post_receive(struct rdma_cm_id *id){
    struct ibv_recv_wr wr,*bad_wr = NULL;
    memset(&wr,0,sizeof(wr));

    //为什么此处没有指任何sge的信息?
    wr.wr_id = (uintptr_t)id;
    wr.sg_list = NULL;
    wr.num_sge = 0;

    TEST_NZ(ibv_post_recv(id->qp,&wr,&bad_wr));
}

static void on_pre_conn(struct rdma_cm_id *id){
    struct conn_context *ctx = (struct conn_context *)malloc(sizeof(struct conn_context));

    id->context = ctx;

    ctx->file_name[0]='\0';//take this to mean we don't have the file name

    posix_memalign((void **)&ctx->buffer,sysconf(_SC_PAGE_SIZE),BUFFER_SIZE);//buffer用来接收数据注册的内存
    TEST_Z(ctx->buffer_mr = ibv_reg_mr(rc_get_pd(),ctx->buffer,BUFFER_SIZE,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

    posix_memalign((void **)&ctx->msg,sysconf(_SC_PAGE_SIZE),sizeof(*ctx->msg));//msg用来发送数据注册的内存
    TEST_Z(ctx->msg_mr = ibv_reg_mr(rc_get_pd(),ctx->msg,sizeof(*ctx->msg),0));

    post_receive(id);//server的post_receive基本等于没有用
}

static void send_message(struct rdma_cm_id *id){
    struct conn_context *ctx = (struct conn_context*)id->context;
    struct ibv_send_wr wr,*bad_wr = NULL;
    struct ibv_sge sge;

    memset(&wr,0,sizeof(wr));

    wr.wr_id = (uintptr_t)id;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = (uintptr_t)ctx->msg;
    sge.length = sizeof(*ctx->msg);
    sge.lkey = ctx->msg_mr->lkey;

    TEST_NZ(ibv_post_send(id->qp,&wr,&bad_wr));

}

static void on_connection(struct rdma_cm_id *id){
    struct conn_context *ctx = (struct conn_context *)id->context;
    //将自己的地址和key发送给对端
    ctx->msg->id = MSG_MR;
    ctx->msg->data.mr.addr = (uintptr_t)ctx->buffer_mr->addr;//即发送msg的数据为自己的本地地址和rkey
    ctx->msg->data.mr.rkey = ctx->buffer_mr->rkey;

    send_message(id);

}

static void on_completion(struct ibv_wc *wc){
    struct rdma_cm_id *id = (struct rdma_cm_id *)(uintptr_t)wc->wr_id;
    struct conn_context *ctx = (struct conn_context *)id->context;

    if(wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM){
        uint32_t size = ntohl(wc->imm_data);
        //msg表示接收的数据
        if(size == 0){
            //如果imma_data为0表示发送端已经发送完数据，此时接收端返回完成
            ctx->msg->id = MSG_DONE;
            send_message(id);
        }else if(ctx->file_name[0]){
            ssize_t ret;
            printf("received %i bytes\n",size);
            ret = write(ctx->fd,ctx->buffer,size);//从ctx->buffer往ctx->fd写size大小的数据

            if(ret!=size){
                rc_die("write() failed\n");
            }
            post_receive(id);
            ctx->msg->id = MSG_READY;
            send_message(id);
        }else {//表示此时收到的是文件名
            size = (size > MAX_FILE_NAME)?MAX_FILE_NAME:size;
            memcpy(ctx->file_name,ctx->buffer,size);

            ctx->file_name[size - 1] = '\0';

            printf("opening file %s \n",ctx->file_name);
            ctx->fd = open(ctx->file_name,O_WRONLY | O_CREAT | O_APPEND , S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

            if(ctx->fd == -1) rc_die("open() failed");

            post_receive(id);

            ctx->msg->id = MSG_READY;
            send_message(id);
        }
    }

}


static void on_disconnect(struct rdma_cm_id *id){
    struct conn_context *ctx = (struct conn_context *)id->context;

    close(ctx->fd);
    ibv_dereg_mr(ctx->buffer_mr);
    ibv_dereg_mr(ctx->msg_mr);

    free(ctx->buffer);
    free(ctx->msg);

    printf("finished transferring %s\n",ctx->file_name);

    free(ctx);
}

int main(int argc, char const *argv[])
{
    rc_init(
        on_pre_conn,
        on_connection,
        on_completion,
        on_disconnect
    );
    printf("waiting for connections.\n");

    rc_server_loop(DEFAULT_PORT);
    return 0;
}
