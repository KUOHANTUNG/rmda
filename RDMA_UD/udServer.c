#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
static int page_size;
static int validate_buf;
static int use_odp;
static int implicit_odp;
static int prefetch_mr;
static int use_dm;
static int	mode;
enum{
    REVC_WR_ID = 1,
    SEND_WR_ID,
	READ_WR_ID,
	WRITE_WR_ID
};
struct data{
	struct ibv_mr 	mr;
	char			txt[256];
};
struct message{
	char head[40];
	struct data dt;
};


struct context {
	struct ibv_context	*context; // dev info
	struct ibv_comp_channel *channel; // complete tunnel
	struct ibv_pd		*pd;// protect domain
	struct ibv_mr		*msg_mr_send;// register memory
	struct ibv_mr		*msg_mr_recv;// register memory
	struct ibv_mr		*mr_read_write;//register memory for read/write
	struct ibv_cq		*cq;// complete channel 
	struct ibv_qp		*qp;// queue pair
	struct ibv_ah		*ah;// address handler
	struct ibv_dm		*dm;//device memory
	struct message		*mes_buf_send;
	struct message		*mes_buf_recv;
	char 			*buf_read_write;    
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo; // ibv port info
};

struct dest {
	int lid;//local identifer
	int qpn;//queue pair number
	int psn;//packet sequence number
	union ibv_gid gid;//GID
};

static int close_ctx(struct context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->msg_mr_send)) {
		fprintf(stderr, "Couldn't deregister msg_mr_send\n");
		return 1;
	}
	if (ibv_dereg_mr(ctx->msg_mr_recv)) {
		fprintf(stderr, "Couldn't deregister msg_mr_recv\n");
		return 1;
	}
	if (ibv_dereg_mr(ctx->mr_read_write)) {
		fprintf(stderr, "Couldn't deregister mr_read_write\n");
		return 1;
	}
	if (ibv_destroy_ah(ctx->ah)) {
		fprintf(stderr, "Couldn't destroy AH\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->mes_buf_send);
	free(ctx->mes_buf_recv);
	free(ctx->buf_read_write);
	free(ctx);

	return 0;
}

static int connect_ctx(struct context *ctx, 
    int port, 
    int my_psn,
	int sl, 
    struct dest *dest, 
    int sgid_idx
){
    struct ibv_ah_attr ah_attr = {
		.is_global     = 0,
		.dlid          = (uint16_t)dest->lid,
		.sl            = (uint8_t)sl,
		.src_path_bits = 0,
		.port_num      = (uint8_t)port
	};
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR
	};

	if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.sq_psn	    = my_psn;

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_SQ_PSN)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	if (dest->gid.global.interface_id) {
		ah_attr.is_global = 1;
		ah_attr.grh.hop_limit = 1;
		ah_attr.grh.dgid = dest->gid;
		ah_attr.grh.sgid_index = sgid_idx;
	}

	ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
	if (!ctx->ah) {
		fprintf(stderr, "Failed to create AH\n");
		return 1;
	}

	return 0;
}

static int rc_connect_ctx(struct context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct dest *dest, int sgid_idx)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= (uint32_t)dest->qpn,
		.rq_psn			= (uint32_t)dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= (uint16_t)dest->lid,
			.sl		= (uint8_t)sl,
			.src_path_bits	= 0,
			.port_num	= (uint8_t)port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

/***
 * change format to wire 
 * 
 * */
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}
/***
 * revert wire format to standard format
 */
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	__be32 v32;
	int i;
	uint32_t tmp_gid[4];

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		tmp_gid[i] = be32toh(v32);
	}
	memcpy(gid, tmp_gid, sizeof(*gid));
}

/**
 * Exhange dest info
 */
static struct dest* client_exch_dest(
    const char *servername, 
    int port,
	const struct dest *my_dest
){
    struct addrinfo *res, *t;
    struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
    char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct dest *rem_dest = NULL;
	char gid[33];

    if (asprintf(&service, "%d", port) < 0)
		return NULL;
    /*auto anlysis the destination info*/
    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}
    /*  build socket connection */
    for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

    if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

    gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
    /**send client gid */
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

    if (read(sockfd, msg, sizeof msg) != sizeof msg ||
	    write(sockfd, "done", sizeof "done") != sizeof "done") {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}

    rem_dest = (struct dest*)malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}

static struct dest *server_exch_dest(
    struct context *ctx,
	int ib_port, int port, int sl,
	const struct dest *my_dest,
	int sgid_idx
){
    /*main difference is that server needs listening */
    struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct dest *rem_dest = NULL;
	char gid[33];

    if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);
    if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}
    t= res;
    while(1){
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
        t = t->ai_next;
    }

    freeaddrinfo(res);
	free(service);

    if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}
    listen(sockfd, 1);
	connfd = accept(sockfd, NULL, NULL);
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

    n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = (struct dest*)malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (connect_ctx(ctx, ib_port, my_dest->psn, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(connfd, msg, sizeof msg) != sizeof msg ||
	    read(connfd, msg, sizeof msg) != sizeof "done") {
		fprintf(stderr, "Couldn't send/recv local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}
out:
	close(connfd);
	return rem_dest;
}


static int post_recv(struct context *ctx, int n,void *addr, uint32_t length, uint32_t key,uint64_t id){
    /*RECV BUFFER*/
    struct ibv_sge list = {
		.addr	= (uintptr_t) addr,
		.length = length,
		.lkey	= key
	};

    struct ibv_recv_wr wr = {
		.wr_id	    = id,
		.sg_list    = &list,
		.num_sge    = 1,
	};

    struct ibv_recv_wr *bad_wr;
	int i;

    for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int post_send(struct context *ctx, 
		uint32_t qpn, 
		int opcode, 
		void *addr, 
		uint32_t length,
		uint32_t key,
		uint64_t id
)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) addr,
		.length = length,
		.lkey	= key
	};
	struct ibv_send_wr wr = {
		.wr_id	    = id,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = opcode,
		.send_flags = (unsigned int)ctx->send_flags,
		.wr         = {
			.ud = {
				 .ah          = ctx->ah,
				 .remote_qpn  = qpn,
				 .remote_qkey = 0x11111111
			 }
		}
	};
	struct ibv_send_wr *bad_wr;
	if(opcode != IBV_WR_SEND){
		wr.wr.rdma.remote_addr = (uintptr_t)ctx->mes_buf_recv->dt.mr.addr;
		wr.wr.rdma.rkey = ctx->mes_buf_recv->dt.mr.rkey;
	}
	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

/****
 * ib_dev: 
 * size:
 * rx_depth:
 * port:
 * use_event:
 */
static struct context *init_ctx(
    struct ibv_device *ib_dev,  
	int size,
    int rx_depth, 
	int port, 
	int use_event
){
    struct context *ctx;
	int access_flags = IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE;
    ctx = (struct context*)malloc(sizeof *ctx);
	if (!ctx)
		return NULL;
    
    ctx->size       = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth   = rx_depth;
    /*align with page_size*/
	ctx->mes_buf_recv = (struct message*)memalign(page_size, sizeof(struct message));
	if (!ctx->mes_buf_recv) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}
    ctx->buf_read_write = (char*)memalign(page_size, size+40);
	if (!ctx->buf_read_write) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_mes_buf_recv;
	}
	ctx->mes_buf_send = (struct message*)memalign(page_size, sizeof(struct message));
	if (!ctx->mes_buf_send) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_buf_read_write;
	}
    memset(ctx->mes_buf_send, 0, sizeof(struct message));
	memset(ctx->mes_buf_recv, 0, sizeof(struct message));
	memset(ctx->buf_read_write, 0, size+40);
    ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_mes_buf_send;
	}


	{
		struct ibv_port_attr port_info = {};
		int mtu;

		if (ibv_query_port(ctx->context, port, &port_info)) {
			fprintf(stderr, "Unable to query port info for port %d\n", port);
			goto clean_device;
		}
		mtu = 1 << (port_info.active_mtu + 7);//active_mtu difference with 7
		if (size > mtu) {
			fprintf(stderr, "Requested size larger than port MTU (%d)\n", mtu);
			goto clean_device;
		}
	}
    /*event driver*/
    if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} else
		ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}
	ctx->mr_read_write = ibv_reg_mr(ctx->pd, ctx->buf_read_write, size+40, access_flags);
	if (!ctx->mr_read_write) {
		fprintf(stderr, "Couldn't register mr_read_write\n");
		goto clean_pd;
	}
	ctx->msg_mr_send = ibv_reg_mr(ctx->pd, ctx->mes_buf_send, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->msg_mr_send) {
		fprintf(stderr, "Couldn't register msg_mr_send\n");
		goto clean_mr_read_write;
	}
	ctx->msg_mr_recv = ibv_reg_mr(ctx->pd, ctx->mes_buf_recv, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->msg_mr_recv) {
		fprintf(stderr, "Couldn't register msg_mr_recv\n");
		goto clean_msg_mr_send;
	}
    ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
    ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_msg_mr_recv;
	}
    /**init qp */
    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = (uint32_t)rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_UD,
		};

        ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}

        ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
        /**if it can be inline, inline it */
		if (init_attr.cap.max_inline_data >= size) {
			ctx->send_flags |= IBV_SEND_INLINE;
		}
    }

    {
        // if(!mode){
			struct ibv_qp_attr attr_ud = {
				.qp_state        = IBV_QPS_INIT,
				.pkey_index      = 0,//default patition
				.port_num        = (uint8_t) port,
				.qkey            = 0x11111111
			};
			if (ibv_modify_qp(ctx->qp, &attr_ud,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_QKEY)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
    }
        return ctx;
    clean_qp:
	ibv_destroy_qp(ctx->qp);

    clean_cq:
	ibv_destroy_cq(ctx->cq);
	clean_msg_mr_recv:
	ibv_dereg_mr(ctx->msg_mr_recv);
    clean_msg_mr_send:
	ibv_dereg_mr(ctx->msg_mr_send);
	clean_mr_read_write:
	ibv_dereg_mr(ctx->mr_read_write);
    clean_pd:
	ibv_dealloc_pd(ctx->pd);
    clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);
    clean_device:
	ibv_close_device(ctx->context);

    clean_mes_buf_send:
	free(ctx->mes_buf_send);
	clean_buf_read_write:
	free(ctx->buf_read_write);
	clean_mes_buf_recv:
	free(ctx->mes_buf_recv);
    clean_ctx:
	free(ctx);

	return NULL;
}


static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 2048)\n");
	printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
    printf("  -l, --sl=<SL>          send messages with service level <SL> (default 0)\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
}


int main(int argc, char* argv[]){
    struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct context *ctx;
	struct dest     my_dest;
	struct dest    *rem_dest;
	struct timeval           start, end;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	unsigned int             port = 18515;
	int                      ib_port = 1;
	unsigned int             size = 1024;
	unsigned int             rx_depth = 500;
	unsigned int             iters = 1000;
	int                      use_event = 0;
	int                      routs;
	int                      rcnt, scnt;
	int                      num_cq_events = 0;
	int                      sl = 0;
	int			 gidx = -1;
	char			 gid[33];

    srand48(getpid() * time(NULL));

    while(1){
        int c;

        static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "size",     .has_arg = 1, .val = 's' },
			{ .name = "rx-depth", .has_arg = 1, .val = 'r' },
			{ .name = "iters",    .has_arg = 1, .val = 'n' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },  
			{}
		};
        c = getopt_long(argc, argv, "p:d:i:s:r:n:l:e:g", long_options,
				NULL);
        
        if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 1) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtoul(optarg, NULL, 0);
			break;

		case 'r':
			rx_depth = strtoul(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtoul(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'e':
			++use_event;
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
		usage(argv[0]);
		return 1;
	}

    page_size = sysconf(_SC_PAGESIZE);//get system page size

    dev_list = ibv_get_device_list(NULL);
    if(!dev_list){
        perror("Failed to get IB devices list");
        return 1;
    }

    if (!ib_devname) {
        //use the default one 
        // first in the list
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	}else {
		int i;
        //loop find the dev
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}
    
    ctx = init_ctx(ib_dev, size, rx_depth, ib_port, use_event);
    if(!ctx)
        return 1;
    routs = post_recv(ctx,ctx->rx_depth,ctx->mes_buf_recv,
			sizeof(struct message),
			ctx->msg_mr_recv->lkey,
			REVC_WR_ID
			);
    if(routs < ctx->rx_depth){
        fprintf(stderr, "Couldn't post receive (%d)\n", routs);
        return 1;
    }

    if(use_event){
        if(ibv_req_notify_cq(ctx->cq, 0)){
            fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
        }
    }

    if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
		    fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}

    my_dest.lid = ctx->portinfo.lid;//only ud mode use
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;

    if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "Could not get local gid for gid index "
								"%d\n", gidx);
			return 1;
		}
	}else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);
    inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);//transfer into ASCII

    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid);
    
    if (servername)
		rem_dest = client_exch_dest(servername, port, &my_dest);
	else
		rem_dest = server_exch_dest(ctx, ib_port, port, sl,
							&my_dest, gidx);
    inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);
    if (servername){
       if (connect_ctx(ctx, ib_port, 
            my_dest.psn, 
            sl, 
            rem_dest,
			gidx)){
               return 1;  
            }		
    }
    /**get current time */
    if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}
    if (servername) {
        pid_t pid = getpid();

        sprintf(ctx->mes_buf_send->dt.txt,
		"HELLO,I am client %d, my_local_key: %d and mem_address: %p\n", 
		pid, 
		ctx->mr_read_write->rkey,
		ctx->mr_read_write->addr
		);
		memcpy(&ctx->mes_buf_send->dt.mr,ctx->mr_read_write,sizeof(struct ibv_mr));
		if (post_send(ctx, rem_dest->qpn,IBV_WR_SEND,
			&(ctx->mes_buf_send->dt),sizeof(struct data),
			ctx->msg_mr_send->lkey,
			SEND_WR_ID
			)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
        scnt++;//client has sent
	}

	for(int j =0; j<2;j++){
		/*event driver*/
    if(use_event){
        struct ibv_cq *ev_cq;
		void          *ev_ctx;
        if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
			fprintf(stderr, "Failed to get cq_event\n");
			return 1;
		}
        ++num_cq_events;
        if (ev_cq != ctx->cq) {
			fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
			return 1;
		}

		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		}

    }
    {
        struct ibv_wc wc[1];
		int ne;
        do {
			ne = ibv_poll_cq(ctx->cq, 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (!use_event && ne < 1);
        for(int i = 0; i < ne; i++){

            if (wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
				ibv_wc_status_str(wc[i].status),
				wc[i].status, (int) wc[i].wr_id);
				return 1;
			}

            switch ((int) wc[i].wr_id){
                case SEND_WR_ID :
                    printf("SUCCESSFUL SEND DATA\n");
                    break;
                case REVC_WR_ID :						
                    printf("RECEIVED DATA FROM REMOTE: %s\n", ctx->mes_buf_recv->dt.txt);
                    if(!scnt){
                        pid_t pid = getpid();
						sprintf(ctx->mes_buf_send->dt.txt,
								"HELLO,I am server %d, my_local_key: %d and mem_address: %p\n", 
								pid, 
								ctx->mr_read_write->rkey,
								ctx->mr_read_write->addr
								);
						memcpy(&ctx->mes_buf_send->dt.mr,ctx->mr_read_write,sizeof(struct ibv_mr));
						if (post_send(ctx, rem_dest->qpn,IBV_WR_SEND,
							&(ctx->mes_buf_send->dt),sizeof(struct data),
							ctx->msg_mr_send->lkey,
							SEND_WR_ID
							)) {
							fprintf(stderr, "Couldn't post send\n");
							return 1;
						}
                        scnt++;//client has sent  
                    }
                    break;
                default:
                    fprintf(stderr, "Completion for unknown wr_id %d\n",
					(int) wc[i].wr_id);
					return 1;
                    break;
            }
        }
            
    }

	}
    ibv_ack_cq_events(ctx->cq, num_cq_events);
    if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		return 1;
	}
    printf("connetion end at %ld\n", end.tv_sec);
    if(close_ctx(ctx))
        return 1;
    ibv_free_device_list(dev_list);
	free(rem_dest);
    return 0;
}
