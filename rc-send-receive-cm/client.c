#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <rdma/rdma_cma.h>

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const int BUFFER_SIZE = 1024;
const int TIMEOUT_IN_MS = 500; /* ms */

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t cq_poller_thread;
};

struct connection {
  struct rdma_cm_id *id;
  struct ibv_qp *qp;

  struct ibv_mr *recv_mr;
  struct ibv_mr *send_mr;

  char *recv_region;
  char *send_region;

  int num_completions;
};

static void die(const char *reason);

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void * poll_cq(void *);
static void post_receives(struct connection *conn);
static void register_memory(struct connection *conn);

static int on_addr_resolved(struct rdma_cm_id *id);
static void on_completion(struct ibv_wc *wc);
static int on_connection(void *context);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);
static int on_route_resolved(struct rdma_cm_id *id);

static struct context *s_ctx = NULL;
struct timeval           start,end1,end2;
float time1 = 0,time2 = 0;

int main(int argc, char **argv)
{
  struct addrinfo *addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *conn= NULL;
  struct rdma_event_channel *ec = NULL;


  if (argc != 3)
    die("usage: client <server-address> <server-port>");

  TEST_NZ(getaddrinfo(argv[1], argv[2], NULL, &addr));

  TEST_Z(ec = rdma_create_event_channel());//打开一个用于报告通信事件的事件通道，异步事件通过事件通道报告给用户


  TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));//创建用于跟踪通信信息的标识符，在概念上类似于套接字
  TEST_NZ(rdma_resolve_addr(conn, NULL, addr->ai_addr, TIMEOUT_IN_MS));//将目标地址和可选源地址从IP地址解析为RDMA地址，如果成功，指定的RDMA_CM_ID将绑定到本地设备

  freeaddrinfo(addr);

  while (rdma_get_cm_event(ec, &event) == 0) {//检索通道事件，如果没有挂起的事件，默认情况下，调用将阻塞，直到收到一个事件
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);//所有报告的事件都必须调用RDMA_ACK_CM_EVENT来确认，直到相关事件得到确认

    if (on_event(&event_copy))
      break;
  }
  rdma_destroy_event_channel(ec);

  return 0;
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void build_context(struct ibv_context *verbs)
{
  if (s_ctx) {
    if (s_ctx->ctx != verbs)
      die("cannot handle events in more than one context.");

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context));

  s_ctx->ctx = verbs;

  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));//创建一个完成通道，完成通道是一种机制，当新的完成队列事件(CQE)被放置在完成队列CQ上时，用户可以接收通知
  TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); //10表示CQE的个数
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));//当CQE在CQ中产生时，一个完成事件将会产生，用户应该使用ibv_get_cq_event操作来接收通知

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = s_ctx->cq;
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;

  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 10;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
}

void * poll_cq(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while (1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 1);//确认从ibv_get_cq_event接收到的事件
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while (ibv_poll_cq(cq, 1, &wc))
      on_completion(&wc);
  }

  return NULL;
}

void post_receives(struct connection *conn)
{
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  sge.addr = (uintptr_t)conn->recv_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->recv_mr->lkey;

  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}

void register_memory(struct connection *conn)
{
  conn->send_region = malloc(BUFFER_SIZE);
  conn->recv_region = malloc(BUFFER_SIZE);

  //注册了两块内存
  TEST_Z(conn->send_mr = ibv_reg_mr(
    s_ctx->pd, 
    conn->send_region, 
    BUFFER_SIZE, 
    0));

  TEST_Z(conn->recv_mr = ibv_reg_mr(
    s_ctx->pd, 
    conn->recv_region, 
    BUFFER_SIZE, 
    IBV_ACCESS_LOCAL_WRITE));
}

int on_addr_resolved(struct rdma_cm_id *id)
{
  struct ibv_qp_init_attr qp_attr;
  struct connection *conn;

  printf("address resolved.\n");

  build_context(id->verbs);//创建除QP和mr以外的其他环境
  build_qp_attr(&qp_attr);//创建QP的属性

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));//创建QP,此时是init

  id->context = conn = (struct connection *)malloc(sizeof(struct connection));//此处的context应该指的是buf，分配内存空间

  conn->id = id;//conn是自己定义的变量
  conn->qp = id->qp;
  conn->num_completions = 0;

  register_memory(conn);//注册内存
  post_receives(conn);//将WQE塞满接收队列

  TEST_NZ(rdma_resolve_route(id, TIMEOUT_IN_MS));//将RDMA路由解析到目的地址，以便建立连接。目标必须已经通过调用RDMA_RESOLVE_ADDR进行了解析。
  //因此，这个函数是在客户端RDMA_RESOLVE_ADDR之后，在调用RDMA_CONNECT之前调用，对于IB连接，调用获得一个由连接使用的路径记录

  return 0;
}

void on_completion(struct ibv_wc *wc)
{
  struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

  if (wc->status != IBV_WC_SUCCESS)
  {
    printf("status is %d, wr_id is %d\n"，wc.status,(int)wc.wr_id);
    die("on_completion: status is not IBV_WC_SUCCESS.");
     
  }

  if (wc->opcode & IBV_WC_RECV)
  {
    printf("received message: %s\n", conn->recv_region);

  }
  else if (wc->opcode == IBV_WC_SEND){
    printf("send completed successfully.\n");
  }
  else{
    die("on_completion: completion isn't a send or a receive.");
  }

  if (++conn->num_completions == 2)
    rdma_disconnect(conn->id);
}

int on_connection(void *context)
{
  struct connection *conn = (struct connection *)context;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  //表示将格式化的数据写入字符串，此处即表示将buffer_size大小的字符串写入conn->send_region
  snprintf(conn->send_region, BUFFER_SIZE, "message from active/client side with pid %d", getpid());

  printf("connected. posting send...\n");

  sge.addr = (uintptr_t)conn->send_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->send_mr->lkey;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
  //表示一次发一个message，从自己的send_region处直接发起一个message
  return 0;
}

int on_disconnect(struct rdma_cm_id *id)
{
  struct connection *conn = (struct connection *)id->context;

  printf("disconnected.\n");

  rdma_destroy_qp(id);

  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->recv_mr);

  free(conn->send_region);
  free(conn->recv_region);

  free(conn);

  rdma_destroy_id(id);

  return 1; /* exit event loop */
}

int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)//代表rdma_resolve_addr成功完成
    r = on_addr_resolved(event->id);//注册一些前期的资源
  else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)//代表rdma_resolve_route路由解析成功完成
    r = on_route_resolved(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)//代表已与远程端点建立连接
    r = on_connection(event->id->context);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)//代表已经断开连接
    r = on_disconnect(event->id);//此处即表示一些数据的销毁
  else
    die("on_event: unknown event.");

  return r;
}

int on_route_resolved(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  printf("route resolved.\n");

  memset(&cm_params, 0, sizeof(cm_params));
  //cm_params表示qp的信息
  TEST_NZ(rdma_connect(id, &cm_params));//发起一个活动连接请求，在调用此方法之前，用户必须已经通过调用了rmda_resolve_route或rdma_create_ep解析到了目标地址的路由
  //调用write向远端发送信息

  return 0;
}
