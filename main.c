#include "common.h"

long long log_head = 0;
long long tot_pipelined = 0;
struct PL_IT *pipeline_out;

// Create protection domain. Create queue pairs and modify them to INIT.
static struct ctrl_blk *init_ctx(struct ctrl_blk *ctx, 
	struct ibv_device *ib_dev)
{
	ctx->context = ibv_open_device(ib_dev);
	CPE(!ctx->context, "Couldn't get context", 0);

	ctx->pd = ibv_alloc_pd(ctx->context);
	CPE(!ctx->pd, "Couldn't allocate PD", 0);

	create_qp(ctx);
	modify_qp_to_init(ctx);

	return ctx;
}

// process_pipeline is called before we want to insert a new request or when we 
// want to add a dummy request.
void process_pipeline(struct ctrl_blk *cb)
{
	int pipeline_index = tot_pipelined & 1;
	int k = 0;
	
	// Move backwards through the pipeline
	for(k = 1; k <= 2; k++) {
		int ind = (pipeline_index - k) & 1;
		int req_type = pipeline[ind].req_type;
        // dummy == fake
		if(req_type == DUMMY_TYPE || req_type == EMPTY_TYPE) {
			if(k == 2) {		// Output the dummy pipeline item
				pipeline_out = &pipeline[ind];
			}
			continue;
		}
		long long *key = pipeline[ind].kv->key;
		int ras = pipeline[ind].req_area_slot;
		
		// Set the polled value in the request region temporarily. Must zero it 
		// out later.
		
		key[KEY_SIZE - 1] = pipeline[ind].poll_val; // should modify by yyt 
		pipeline[ind].kv->len = pipeline[ind].kv_len;
				
		int key_bkt_num = KEY_TO_BUCKET(key[0]);// bucket index
		int key_tag = KEY_TO_TAG(key[0]);// identity (unique in one bucket)

		// Access the bucket as an array of 8 longs
		LL *key_bkt = (LL *) &ht_index[key_bkt_num];// hash_table (1st random access) 

		if(k == 1) {
			if(req_type == PUT_TYPE) {	/*PUT*/
				// If there is ANY chance that the written KV will overflow the log, reset
				if((log_head & LOG_SIZE_) >= LOG_SIZE - 4096) {
					log_head = log_head + (LOG_SIZE - (log_head & LOG_SIZE_));
					fprintf(stderr, "Server %d resetting log head to 0\n",
						cb->id);
				}

				int slot;
				LL max_diff = MIN_LL, best_slot = 0;
				for(slot = SLOTS_PER_BKT - 1; slot >= 1; slot--) {
					if(key_bkt[slot] == INVALID_SLOT) {
						best_slot = slot;
						break;
					}
					LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);// calculate priority for LRU
					if(log_head - log_offset > max_diff) {
						max_diff = log_head - log_offset;
						best_slot = slot;
					}
					// While insertion, we remove collisions.
					int slot_tag = SLOT_TO_TAG(key_bkt[slot]);// same as KEY_TO_TAG()
					if(slot_tag == key_tag) {
						// by yyt 
						key_bkt[slot] = INVALID_SLOT;// why not just store index in key_bkt[slot]???
						//best_slot = slot; break;
					}
				}

				// Prepare the slot. Assuming that log_head is less than 2^48,
				// the offset stored in the slot is in [0, 2^48). 
				key_bkt[best_slot] = key_tag;
				key_bkt[best_slot] |= (log_head << 16);// time tag
			
				// Append to log  
				memcpy(&ht_log[log_head & LOG_SIZE_], pipeline[ind].kv, S_KV);
                //no inplace modify, to reduce random access, but increase memory gap
				log_head += S_KV;
			} else {	/*GET*/
				int slot, key_in_index = 0;
				
				pipeline[ind].get_slot = -1;// add by yyt
				
				for(slot = SLOTS_PER_BKT - 1; slot >= 0; slot--) {// why to 0? 
					int slot_tag = SLOT_TO_TAG(key_bkt[slot]);// random access
					if(slot_tag == key_tag) {
						LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);
						if(log_head - log_offset > LOG_SIZE) {// be covered and recycled, can't find out
							break;
						}
						__builtin_prefetch(&ht_log[log_offset & LOG_SIZE_], 0, 3);// prefetch hash_table log

						key_in_index = 1;
						pipeline[ind].get_slot = slot;// store result
						break;
					}
				}
				if(key_in_index == 0) {
					server_resp_area[ras].len = GET_FAIL_LEN_1;// not found in index : 20001
				}
			}
		}	// END 1st pipeline stage
		if(k == 2) {
			if(req_type == GET_TYPE && pipeline[ind].get_slot != -1) {		// GET
				int slot = pipeline[ind].get_slot;// the id in bkt which was got at previous stage(k==1)
				int key_still_in_index = 0, key_in_log = 0;// important
				
				int slot_tag = SLOT_TO_TAG(key_bkt[slot]);// have been accessed
				if(slot_tag == key_tag) {// why check again??? because may not be found, so the slot are old.
					key_still_in_index = 1;
					LL log_offset = SLOT_TO_OFFSET(key_bkt[slot]);
					LL log_addr = log_offset & LOG_SIZE_;
					
					LL *log_key = (LL *) &ht_log[log_addr + KV_KEY_OFFSET];    // fetch : yyt
					int valid = (log_key[0] == key[0]);// check the key match, may be covered when k==1
					#if(KEY_SIZE == 2)
						valid &= (log_key[1] == key[1]);
					#endif
					
					//int valid = log_head - log_offset <= LOG_SIZE;
					if(valid) {
						key_in_log = 1;
						// Copy the log straight to the response and record
						// that this has been done.
						SET_PL_IT_MEMCPY_DONE(pipeline[ind]);// add tag, means complete
						memcpy((char *) &server_resp_area[ras], &ht_log[log_addr], S_KV); // set response
						if((char)server_resp_area[ras].key[0] == 0 || server_resp_area[ras].len == 0){
							fprintf(stderr, "server resp key/len = 0\n");
							exit(1);
						}
					}
				}
				if(key_still_in_index == 0) {
					server_resp_area[ras].len = GET_FAIL_LEN_1;// not found in log : 20002
				}
				else if(key_still_in_index == 1 && key_in_log == 0) {
					server_resp_area[ras].len = GET_FAIL_LEN_2;// not found in log : 20002
				}
			}
			pipeline_out = &pipeline[ind];// result
		}
		key[KEY_SIZE - 1] = 0;	// should modify by yyt 
		pipeline[ind].kv->len = 0;
	}
}

void run_server(struct ctrl_blk *cb)
{
	if(cb->id == 0) {// 0th process one server do nothing (only creat QP which receives RDMA WRITE)
        while(1){
            /*
			for(int i=0;i<REQ_MEMSIZE;i++){
                fprintf(stderr, "%d:%d ", i, (int)((char*)server_req_area)[i]);
            }
            fprintf(stderr, "\n");
			*/
            sleep(1);
        }
    }

	struct ibv_send_wr *bad_send_wr;

	int i, ret = 0, num_resp = 0;
	int last_resp = -1;
	struct timespec start, end;		// Timers for throughput
	
	int req_lo[NUM_CLIENTS];		// Base request index for each client
	int req_num[NUM_CLIENTS];		// Offset above the base index

	int failed_polls = 0;			// # of failed attempts to find a new request
	init_ht(cb);

	for(i = 0; i < 2; i++) {
		pipeline[i].req_type = EMPTY_TYPE;
	}

	for(i = 0; i < NUM_CLIENTS; i++) {// one window contains W entries for one client 
		req_lo[i] = (cb->id * (WINDOW_SIZE * NUM_CLIENTS)) + (i * WINDOW_SIZE);
		req_num[i] = 0;
	}

	clock_gettime(CLOCK_REALTIME, &start);
	for(int cc = 0 ; ; cc++) {
		for(i = 0; i < NUM_CLIENTS; i++) {
			// usleep(200000);
			if((num_resp & M_1_) == M_1_ && num_resp > 0 && num_resp != last_resp) {
				clock_gettime(CLOCK_REALTIME, &end);
				double seconds = (end.tv_sec - start.tv_sec) +
					(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
				fprintf(stderr, "Server %d, IOPS: %f, used fraction: %f\n", 
					cb->id, M_1 / seconds, (double) log_head / LOG_SIZE);
				clock_gettime(CLOCK_REALTIME, &start);
				last_resp = num_resp;
			}

			// Poll for a new request
			int req_ind = req_lo[i] + (req_num[i] & WINDOW_SIZE_);//offset = req_num % window_size
            if(cc % M_128 == 0){
                debug(stderr, "CHECK addr %lld\n", (LL)&server_req_area[req_ind]);
                debug(stderr, "OFFSET id %lld\n", (LL)req_ind);
            }
			if((char) server_req_area[req_ind].key[KEY_SIZE - 1] == 0) {
				failed_polls ++;
				if(failed_polls < FAIL_LIM) {//failed_polls == 100 -> send "nope"
					continue;
				}
			}
			if((char) server_req_area[req_ind].key[KEY_SIZE - 1] != 0){//add by yyt
				debug(stderr, "Server %d, receive a request\n", cb->id);
			}
			// Issue prefetches before computation
			if(failed_polls < FAIL_LIM) {
				int key_bkt_num = KEY_TO_BUCKET(server_req_area[req_ind].key[0]);
				
				// We only get here if we find a new valid request. Therefore,
				// it's OK to use the len field to determine request type
				if(server_req_area[req_ind].len > 0) {
					__builtin_prefetch(&ht_index[key_bkt_num], 1, 3);// prefetch hash_table index
				} else {
					__builtin_prefetch(&ht_index[key_bkt_num], 0, 3);
				}
			}
			debug(stderr, "before pipe\n");
			process_pipeline(cb);
			debug(stderr, "after pipe\n");
			
			// Process the pipeline's output. pipeline_out is a pointer to a
			// pipeline slot. The new request will get pushed into this slot.
			// Process the output *before* pushing the new request in.

			// Is the output legit?
			if(pipeline_out->req_type != DUMMY_TYPE && pipeline_out->req_type != EMPTY_TYPE) {	
				int cn = pipeline_out->cn & 0xff;// get client number only
				int ras = pipeline_out->req_area_slot;

				cb->wr.wr.ud.ah = cb->ah[cn];
				cb->wr.wr.ud.remote_qpn = cb->remote_dgram_qp_attrs[cn].qpn;
				cb->wr.wr.ud.remote_qkey = 0x11111111;// interesting

				cb->wr.send_flags = (num_resp & WS_SERVER_) == 0 ?
					MY_SEND_INLINE | IBV_SEND_SIGNALED : MY_SEND_INLINE;// inline as well
				if((num_resp & WS_SERVER_) == WS_SERVER_) {
					poll_dgram_cq(1, cb, 0);
				}

				if(pipeline_out->req_type == PUT_TYPE) {		// PUT response
					cb->sgl.addr = (uint64_t) (unsigned long) &server_resp_area[ras]; // set in pipeline
					cb->wr.sg_list->length = 1;// return 1 byte? what's in it? zero?
				} else if(pipeline_out->req_type == GET_TYPE) {
					cb->sgl.addr = (uint64_t) (unsigned long) &server_resp_area[ras];
					cb->wr.sg_list->length = KV_KEY_OFFSET;// return length and value, wrong number will set in length
				} else {
					fprintf(stderr, "No type?!\n");
					exit(0);
				}
				cb->wr.next = NULL;// bug again
				
				
				
				debug(stderr, "before send a RESP back\n");
				ret = ibv_post_send(cb->dgram_qp[0], &cb->wr, &bad_send_wr);
				debug(stderr, "after send a RESP back\n");
				CPE(ret, "ibv_post_send error", ret);
				
				num_resp++;
			}

			// Add a new request (legit/dummy) into the pipeline
			// The index in the pipeline where the new item will be pushed
			int pipeline_index = tot_pipelined & 1;

			if(failed_polls < FAIL_LIM) {// a new REQ
				if(server_req_area[req_ind].len == 0) {
					pipeline[pipeline_index].req_type = GET_TYPE;
				} else {
					pipeline[pipeline_index].req_type = PUT_TYPE;
				}

				pipeline[pipeline_index].kv = (struct KV *) &server_req_area[req_ind];
				pipeline[pipeline_index].cn = i;
				pipeline[pipeline_index].req_area_slot = req_ind;// the id in req_area

				// Store the polled value in the pipeline item and make it zero
				// in the request region
				pipeline[pipeline_index].poll_val = server_req_area[req_ind].key[KEY_SIZE - 1];//
				pipeline[pipeline_index].kv_len = server_req_area[req_ind].len;//
				// Zero out the polled key so that this request is not detected again.
				// Make the len field zero. If a new request is detected an len is
				// still 0, it means that the new request is a GET.
				
				
				// modified by yyt, set 0 later
				server_req_area[req_ind].key[KEY_SIZE - 1] = 0;
				server_req_area[req_ind].len = 0;// must be set to 0, because GET only give key

				req_num[i] ++;
			} else {
				pipeline[pipeline_index].req_type = DUMMY_TYPE;// nope
				failed_polls = 0;
			}

			tot_pipelined ++;
		}
	}
	return;
}

// Post a recv() for a send() from server sn
void post_recv(struct ctrl_blk *cb, int iter_, int sn)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) &client_resp_area[(sn * WINDOW_SIZE) + iter_],// right position of resp_area
		.length = S_UD_KV,
		.lkey	= client_resp_area_mr->lkey
	};

	// This does not use the wr in cb - avoids interference
	// with the WRITE to server
	struct ibv_recv_wr recv_wr = {
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int ret = ibv_post_recv(cb->dgram_qp[sn], &recv_wr, &bad_wr);
	if(ret) {
		fprintf(stderr, "Error %d posting recv.\n", ret);
		exit(0);
	}
}

void run_client(struct ctrl_blk *cb)
{
	struct ibv_send_wr *bad_send_wr;
	struct ibv_wc wc[WS_SERVER];
	
	struct timespec start, end;		// Throughput timers
	struct timespec op_start[WINDOW_SIZE], op_end[WINDOW_SIZE];	// Latency timers
	uint64_t fastrand_seed = 0xdeadbeef;
	LL total_nsec = 0;

	fprintf(stderr, "Starting client %d\n", cb->id);
	clock_gettime(CLOCK_REALTIME, &start);

	int ret, iter = 0, sn = -1;
	int num_resp = 0, num_req = 0, wait_cycles = 0, num_fails = 0;

	// Number of pending requests and responses received from each server
	int num_req_arr[NUM_SERVERS];	
	memset(num_req_arr, 0, NUM_SERVERS * sizeof(int));

	int num_resp_arr[NUM_SERVERS];	
	memset(num_resp_arr, 0, NUM_SERVERS * sizeof(int));

	// The server contacted and the key used in a window slot
	int sn_arr[WINDOW_SIZE];		// Required for polling for recv comps
	memset(sn_arr, 0, WINDOW_SIZE * sizeof(int));
	
	LL pndng_keys[WINDOW_SIZE];		// The keys for which a response is pending
	memset(pndng_keys, 0, WINDOW_SIZE * sizeof(LL));

	// Generate the keys to be requested
	int key_i = 0;
	srand48(cb->id);
	LL *key_corpus = gen_key_corpus(cb->id);

	// Pre-post some RECVs in slot order for the servers
	debug(stderr, "in post_recv\n");
	int serv_i;
	for(serv_i = 1; serv_i < NUM_SERVERS; serv_i ++) {
		int recv_i;
		for(recv_i = 0; recv_i < CL_BTCH_SZ; recv_i ++) {
			post_recv(cb, recv_i & WINDOW_SIZE_, serv_i);
		}
	}
	debug(stderr, "out\n");

	for(iter = 0; iter < NUM_ITER; iter++) {
		// usleep(200000);
		int iter_ = iter & WINDOW_SIZE_;
		volatile struct KV *req_kv = &client_req_area[iter_];

		// Performance measurement
		if((iter & M_1_) == M_1_ && iter != 0) {
			fprintf(stderr, "\nClient %d completed %d ops\n", cb->id, iter);
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) +
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

			fprintf(stderr, "IOPS = %f\n", M_1 / seconds);
			
			double sgl_read_time = (double) total_nsec / M_1;
			fprintf(stderr, "Average op time = %f us\n", sgl_read_time / 1000);
			total_nsec = 0;
			fprintf(stderr, "Avg wait = %f, avg fail = %f\n", 
				(double) wait_cycles / M_1,
				(double) num_fails / M_1_);
			wait_cycles = 0;
			num_fails = 0;

			clock_gettime(CLOCK_REALTIME, &start);
		}

		// First, we PUT all our keys.
		if(rand() % 100 < PUT_PERCENT || iter < NUM_KEYS) {// 2^20
			req_kv->key[0] = key_corpus[key_i];
			#if(KEY_SIZE == 2)
				req_kv->key[1] = key_corpus[key_i];
			#endif
			req_kv->len = VALUE_SIZE;
			memset((char *) req_kv->value, (char) key_corpus[key_i], VALUE_SIZE);
			pndng_keys[iter_] = 0;// ???
			key_i = (key_i + 1) & NUM_KEYS_;
		} else {
			key_i = rand() & NUM_KEYS_;
			req_kv->key[0] = key_corpus[key_i];
			#if(KEY_SIZE == 2)
				req_kv->key[1] = key_corpus[key_i];
			#endif
			req_kv->len = 0;
			memset((char *) req_kv->value, 0, VALUE_SIZE);// every value is OK
			pndng_keys[iter_] = req_kv->key[0];// ???
		}
		
		sn = KEY_TO_SERVER(req_kv->key[0]);// [1, num_servers - 1]
		sn_arr[iter_] = sn;

		int req_offset = (sn * WINDOW_SIZE * NUM_CLIENTS) + 
			(WINDOW_SIZE * cb->id) + (num_req_arr[sn] & WINDOW_SIZE_);

		clock_gettime(CLOCK_REALTIME, &op_start[iter_]);
		
		cb->wr.send_flags = (num_req & S_DEPTH_) == 0 ?		// ??? yyt : 
			MY_SEND_INLINE | IBV_SEND_SIGNALED : MY_SEND_INLINE;// at creat_qp we have set max_inline_data 
																// inline data will be stored in WQE
																// lkey will not checked in this modulo
																// even don't need to register a memory region
		if((num_req & S_DEPTH_) == S_DEPTH_) {				// every 512 responses, clear CQ and send a ACK
			poll_conn_cq(1, cb, 0);
		}

		// Real work
		if(req_kv->len == 0) {		// GET
			cb->sgl.addr = (uint64_t) (unsigned long) &req_kv->key;
			cb->wr.sg_list->length = S_KV - KV_KEY_OFFSET;
			cb->wr.wr.rdma.remote_addr = server_req_area_stag[0].buf + 
				(req_offset * S_KV) + KV_KEY_OFFSET;
		} else { // PUT
			cb->sgl.addr = (uint64_t) (unsigned long) req_kv;
			cb->wr.sg_list->length = S_KV;
			cb->wr.wr.rdma.remote_addr = server_req_area_stag[0].buf + 
				(req_offset * S_KV);
		}// in HERD every server_req_area_stag[i].buf is the same
		debug(stderr, "WRITE addr %lld\n", (LL) cb->wr.wr.rdma.remote_addr);
        debug(stderr, "OFFSET id %lld\n",(LL) req_offset);
        
		cb->wr.wr.rdma.rkey = server_req_area_stag[0].rkey;
		
		//cb->wr.sg_list->lkey = client_req_area_mr -> lkey;//already have done
		
		cb->wr.next = NULL;											// huge bug fixed, id = 2
		cb->wr.wr_id = num_req;											// useful???
		// Although each client has NUM_SERVERS conn_qps, they only issue RDMA
		// WRITEs to the 0th server
		debug(stderr, "before ibv_post_send\n");
		ret = ibv_post_send(cb->conn_qp[0], &cb->wr, &bad_send_wr);// the rest queues maybe used in CREW modulo
		debug(stderr, "after ibv_post_send\n"); 
		                                                          // the num of queue (in use) is NUM_CLIENT
        // server shares memory, so need only one QP for WRITE.
        // SEND/WRITE/READ is all through this by set wr.opcode
		CPE(ret, "ibv_post_send error", ret);

		num_req_arr[sn]++;
		num_req ++;

		if(num_req - num_resp == WINDOW_SIZE) {   // remain 4 in flight
			int rws = num_resp & WINDOW_SIZE_;		// Response window slot
			int rsn = sn_arr[rws];					// Response server number
			int ras = (rsn * WINDOW_SIZE) + (num_resp_arr[rsn] & WINDOW_SIZE_);

			// Poll for the recv
			int recv_comps = 0;
			while(recv_comps == 0) {
				wait_cycles ++;
				if(wait_cycles % M_128 == 0) {
					fprintf(stderr, "Wait for iter %d at client %d GET = %lld\n", 
						num_resp + 1, cb->id, pndng_keys[rws]);
				}
				recv_comps = ibv_poll_cq(cb->dgram_cq[rsn], 1, wc);// clear complete queue
			}
			if(wc[0].status != 0) {
				fprintf(stderr, "Bad recv wc status %d\n", wc[0].status);
				exit(0);
			}
			debug(stderr, "client receive a RESP\n");
		
			// If it was a GET, and it succeeded, check it!
			if(pndng_keys[rws] != 0) {
				if(client_resp_area[ras].kv.len < GET_FAIL_LEN_1) {
					if(!valcheck(&client_resp_area[ras].kv,           // check value
						pndng_keys[rws])) {
						fprintf(stderr, "Client %d get() failed in iter %d. ", 
							cb->id, num_resp);
						print_ud_kv(client_resp_area[ras]);
						exit(0);
					}
				}
			}

			if(client_resp_area[ras].kv.len >= GET_FAIL_LEN_1) {
				num_fails ++;
			}
			
			// Batched posting of RECVs
			num_resp_arr[rsn] ++;

			// Recvs depleted: post some more.
			if((num_resp_arr[rsn] & CL_SEMI_BTCH_SZ_) == 0) {
				int recv_i;
				for(recv_i = 0; recv_i < CL_SEMI_BTCH_SZ; recv_i ++) {
					post_recv(cb, recv_i & WINDOW_SIZE_, rsn);     // prepare for next SEND
				}
			}

			memset((char *) &client_resp_area[ras], 0, sizeof(struct UD_KV));

			clock_gettime(CLOCK_REALTIME, &op_end[rws]);                     // calculate latency
			LL new_nsec = (op_end[rws].tv_sec - op_start[rws].tv_sec)* 1000000000 
				+ (op_end[rws].tv_nsec - op_start[rws].tv_nsec);
			total_nsec += new_nsec;

			if(CLIENT_PRINT_LAT == 1) {	// Print latency so that we can compute percentiles
				if((fastrand(&fastrand_seed) & 0xff) == 0) {
					printf("%lld\n", new_nsec);
				}
			}
			num_resp ++;
		}
	}
	return;
}

/* Usage:
 * Server: sudo ./main <sock_port> <id>
 * Client: sudo ./maun <sock_port> <id> <server_ip>
 */ 
void help(){
	puts("use \"./run.sh [$id] ([$SERVER_IP]) [$START_PORT] [$ENABLE_ROCE = 0/1]\"");
}
int main(int argc, char *argv[])
{
	if(argc < 4 || argc > 5){
		help();
		return 0;
	}
	server_id = atoi(argv[1]);
	int pid = (int) getpid();
	fprintf(stderr,"pid: %d\n",pid);
	FILE *kill_all = fopen("kill_all.sh",server_id?"a":"w");
	if(server_id == 0 && argc == 4)
		fprintf(kill_all, "sudo ipcrm -M %d\n", REQ_AREA_SHM_KEY);
	fprintf(kill_all, "sudo kill -9 %d\n", pid);
	fclose(kill_all);
	
	int i;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct ctrl_blk *ctx;

	srand48(getpid() * time(NULL));		// Required for PSN
	ctx = malloc(sizeof(struct ctrl_blk));
	
	ctx->id = server_id;

	// Allocate space for queue-pair attributes
	if (argc == 5) {
		server_ip = argv[2];
		start_port = atoi(argv[3]);// global
		enable_roce = atoi(argv[4]);// global
		ctx->is_client = 1;
		ctx->num_conn_qps = NUM_SERVERS;
		ctx->num_remote_dgram_qps = NUM_SERVERS;
		ctx->num_local_dgram_qps = NUM_SERVERS;
        /* 
           Why not 1? 
           Maybe because, when receiving SEND from different SERVER, 
           their is conflict both in queue and in memory.
           So, just like the server mechine has (in fact shares) NUM_CLIENT in-use queues(others not in-use), 
           a client should has NUM_SERVER queues.
           However, when sending, their is no conflict so 1 is OK.
           (So every client use one connected queue for WRITE and all server share a UD response-queue)
        */
		ctx->local_conn_qp_attrs = (struct qp_attr *) malloc(NUM_SERVERS * S_QPA);
		ctx->remote_conn_qp_attrs = (struct qp_attr *) malloc(NUM_SERVERS * S_QPA);
		
		// The clients don't need an address handle for the servers UD QPs
		ctx->local_dgram_qp_attrs = (struct qp_attr *) malloc(NUM_SERVERS * S_QPA);
	} else {
		
		cpu_set_t mask;		// big improvement
        CPU_ZERO(&mask);    //置空
        CPU_SET(ctx->id * 2 % 20, &mask);   //设置亲和力值
        if (sched_setaffinity(0, sizeof(mask), &mask) == -1)//设置线程CPU亲和力
        {
            fprintf(stderr, "warning: could not set CPU affinity, continuing...\n");
        }
		
		
		start_port = atoi(argv[2]);// global
		enable_roce = atoi(argv[3]);// global
		ctx->is_client = 0; // huge bug fixed, id = 1
		ctx->sock_port = start_port + ctx->id; 
		ctx->num_conn_qps = NUM_CLIENTS;
		ctx->num_remote_dgram_qps = NUM_CLIENTS;
		ctx->num_local_dgram_qps = 1;

		ctx->local_conn_qp_attrs = (struct qp_attr *) malloc(NUM_CLIENTS * S_QPA);
		ctx->remote_conn_qp_attrs = (struct qp_attr *) malloc(NUM_CLIENTS * S_QPA);
	
		ctx->local_dgram_qp_attrs = (struct qp_attr *) malloc(S_QPA);
		ctx->remote_dgram_qp_attrs = (struct qp_attr *) malloc(NUM_CLIENTS * S_QPA);
	}
	
	// Get an InfiniBand/RoCE device
	dev_list = ibv_get_device_list(NULL);
	CPE(!dev_list, "Failed to get IB devices list", 0);

    

	//ib_dev = dev_list[is_roce() == 1 ? 1 : 0];//服务器上有 [0] = mlx5_1, [1] = mlx5_0 两个 
	//ib_dev = ctx->is_client? dev_list[ctx->id & 1] : dev_list[0]; //always slower
	ib_dev = dev_list[0]; 
	
	CPE(!ib_dev, "IB device not found", 0);

	// Create queue pairs and modify them to INIT
	init_ctx(ctx, ib_dev);
	CPE(!ctx, "Init ctx failed", 0);

	// Create RDMA (request and response) regions 
	setup_buffers(ctx);

	union ibv_gid my_gid = get_gid(ctx->context);

	// Collect local queue pair attributes
	// prepare for "INIT -> RTR" 
	for(i = 0; i < ctx->num_conn_qps; i++) {
		ctx->local_conn_qp_attrs[i].gid_global_interface_id = 
			my_gid.global.interface_id;
		ctx->local_conn_qp_attrs[i].gid_global_subnet_prefix = 
			my_gid.global.subnet_prefix;

		ctx->local_conn_qp_attrs[i].lid = get_local_lid(ctx->context);// 注意这个attr struct是作者写的 
		ctx->local_conn_qp_attrs[i].qpn = ctx->conn_qp[i]->qp_num;// QP number
		ctx->local_conn_qp_attrs[i].psn = lrand48() & 0xffffff;// starting receive packet sequence number
		fprintf(stderr, "Local address of conn QP %d: ", i);
		print_qp_attr(ctx->local_conn_qp_attrs[i]);
	}

	for(i = 0; i < ctx->num_local_dgram_qps; i++) {
		ctx->local_dgram_qp_attrs[i].gid_global_interface_id = 
			my_gid.global.interface_id;
		ctx->local_dgram_qp_attrs[i].gid_global_subnet_prefix = 
			my_gid.global.subnet_prefix;

		ctx->local_dgram_qp_attrs[i].lid = get_local_lid(ctx->context);
		ctx->local_dgram_qp_attrs[i].qpn = ctx->dgram_qp[i]->qp_num;
		ctx->local_dgram_qp_attrs[i].psn = lrand48() & 0xffffff;
		fprintf(stderr, "Local address of dgram QP: %d", i);
		print_qp_attr(ctx->local_dgram_qp_attrs[i]);
	}

	// Exchange queue pair attributes
	fprintf(stderr,"%s\n",ctx->is_client?"in exch_client":"in exch_server");
	if(ctx->is_client) {
		client_exch_dest(ctx);
	} else {
		server_exch_dest(ctx); // and modify connected queues to RTR & RTS
	}
	fprintf(stderr,"out\n");
	// The server creates address handles for every clients' UD QP
	if(!ctx->is_client) {
		for(i = 0; i < NUM_CLIENTS; i++) {
			fprintf(stderr, "Server %d: create_ah for client %d\n", ctx->id, i);
			print_qp_attr(ctx->remote_dgram_qp_attrs[i]);
			struct ibv_ah_attr ah_attr = {
				.is_global		= (is_roce() == 1) ? 1 : 0,
				.dlid			= (is_roce() == 1) ? 0 : ctx->remote_dgram_qp_attrs[i].lid,
				.sl				= 0,
				.src_path_bits	= 0,
				.port_num		= IB_PHYS_PORT
			};

			if(is_roce()) {
				ah_attr.grh.dgid.global.interface_id = 
					ctx->remote_dgram_qp_attrs[i].gid_global_interface_id;
				ah_attr.grh.dgid.global.subnet_prefix = 
					ctx->remote_dgram_qp_attrs[i].gid_global_subnet_prefix;
			
				ah_attr.grh.sgid_index = 0;
				ah_attr.grh.hop_limit = 1;
			}

			ctx->ah[i] = ibv_create_ah(ctx->pd, &ah_attr);
			CPE(!ctx->ah[i], "Failed to create ah", i);
		}
	}
	fprintf(stderr,"in modify_dgram_qp_to_rts\n");
	modify_dgram_qp_to_rts(ctx);
	fprintf(stderr,"out\n");
	// Move the client's connected QPs through RTR and RTS stages
	if (ctx->is_client) {
		for(i = 0; i < NUM_SERVERS; i++) {
			if(connect_ctx(ctx, ctx->local_conn_qp_attrs[i].psn, 
				ctx->remote_conn_qp_attrs[i], i)) {
				return 1;
			}
		}
	}
	fprintf(stderr,"%s\n",ctx->is_client?"in run_client":"in run_server");
	if(ctx->is_client) {
		run_client(ctx);
	} else {
		run_server(ctx);
	}
	fprintf(stderr,"exited seccussfully\n");
	return 0;
}
