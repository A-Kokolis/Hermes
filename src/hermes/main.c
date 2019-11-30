#define _GNU_SOURCE
#include <stdio.h>
#include <malloc.h>
#include <infiniband/verbs.h>
#include <getopt.h>
#include <pthread.h>
#include "spacetime.h"
#include "config.h"
#include "util.h"
#include "../../include/utils/concur_ctrl.h"
#include "hrd.h"
#include "../../include/utils/bit_vector.h"
#include "../../include/wings/wings_api.h"


//Global vars
struct latency_counters latency_count;
volatile struct worker_stats w_stats[WORKERS_PER_MACHINE];

dbit_vector_t *g_share_qs_barrier;

// Global config vars
uint8_t is_CR;
int num_workers;
int update_ratio;
int rmw_ratio;
int credits_num;
int max_coalesce;
int max_batch_size; //for batches to KVS



// This is required only when Hades failure detection is disabled
void group_membership_init(void)
{
    group_membership.num_of_alive_remotes = REMOTE_MACHINES;
    seqlock_init(&group_membership.lock);
    bv_init((bit_vector_t*) &group_membership.g_membership);

    for(uint8_t i = 0; i < MACHINE_NUM; ++i)
        bv_bit_set((bit_vector_t*) &group_membership.g_membership, i);

    bv_copy((bit_vector_t*) &group_membership.w_ack_init, group_membership.g_membership);
    bv_reverse((bit_vector_t*) &group_membership.w_ack_init);
    bv_bit_set((bit_vector_t*) &group_membership.w_ack_init, (uint8_t) machine_id);
}



int main(int argc, char *argv[])
{
	int i, c;
	is_roce = -1; machine_id = -1;

	// config vars
	is_CR = 1;
	num_workers = -1;
	update_ratio = -1;
    rmw_ratio = -1;
	credits_num = -1;
	max_coalesce = -1;
	max_batch_size = -1;
	remote_IP = (char *) malloc(16 * sizeof(char));

//	green_printf("UD size: %d ibv_grh + crd size: %d \n", sizeof(ud_req_crd_t), sizeof(struct ibv_grh) + sizeof(spacetime_crd_t));
//	static_assert(sizeof(ud_req_crd_t) == sizeof(struct ibv_grh) + sizeof(spacetime_crd_t), ""); ///CRD --> 48 Bytes instead of 43


	struct thread_params *param_arr;
	pthread_t *thread_arr;

	static struct option opts[] = {
			{ .name = "machine-id",			.has_arg = 1, .val = 'm' },
			{ .name = "is-roce",			.has_arg = 1, .val = 'r' },
            { .name = "rmw-ratio",			.has_arg = 1, .val = 'R' },
			{ .name = "dev-name",			.has_arg = 1, .val = 'd' },
			{ .name = "write-ratio",		.has_arg = 1, .val = 'w' },
			{ .name = "num-workers",		.has_arg = 1, .val = 'W' },
			{ .name = "credits",		    .has_arg = 1, .val = 'c' },
			{ .name = "max-coalesce",		.has_arg = 1, .val = 'C' },
			{ .name = "max-batch-size",		.has_arg = 1, .val = 'b' },
            { .name = "hermes",		        .has_arg = 0, .val = 'H' },
			{ 0 }
	};

	/* Parse and check arguments */
	while(1) {
		c = getopt_long(argc, argv, "m:r:R:d:w:c:C:W:H", opts, NULL);
		if(c == -1) break;

		switch (c) {
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 'r':
				is_roce = atoi(optarg);
				break;
			case 'd':
				memcpy(dev_name, optarg, strlen(optarg));
				break;
			// Config vars
			case 'w':
				update_ratio = atoi(optarg);
				break;
            case 'R':
                rmw_ratio = atoi(optarg);
                break;
			case 'W':
				num_workers = atoi(optarg);
				break;
			case 'c':
				credits_num = atoi(optarg);
				break;
			case 'C':
				max_coalesce = atoi(optarg);
				break;
			case 'b':
				max_batch_size = atoi(optarg);
				break;
            case 'H':
                is_CR = 0;
                break;
			default:
				printf("Invalid argument %d\n", c);
				assert(false);
		}
	}

	// If arguments not passed use the default values from header file
	if(update_ratio == -1) update_ratio = UPDATE_RATIO;
    if(rmw_ratio == -1) rmw_ratio = ENABLE_RMWs ? RMW_RATIO : 0;
	if(num_workers == -1) num_workers = WORKERS_PER_MACHINE;
	if(credits_num == -1) credits_num = CREDITS_PER_REMOTE_WORKER;
	if(max_coalesce == -1) max_coalesce = MAX_REQ_COALESCE;
	if(max_batch_size == -1) max_batch_size = MAX_BATCH_OPS_SIZE;


	assert(ENABLE_RMWs || rmw_ratio == 0);
    assert(rmw_ratio != 0 || ENABLE_RMWs == 0);
	// WARNING: Some structs are statically allocated using WORKERS_PER_MACHINE / MAX_BATCH_OPS_SIZE
	assert(num_workers <= WORKERS_PER_MACHINE);
	assert(max_batch_size <= MAX_BATCH_OPS_SIZE);

	if(num_workers > 1)
		dbv_init(&g_share_qs_barrier, (uint8_t) num_workers);
	else
		g_share_qs_barrier = NULL;


	printf("update rate: %d (RMW rate %d) | workers %d | batch size %d| CREDITS %d | coalesce %d |\n",
			update_ratio, rmw_ratio, num_workers, max_batch_size, credits_num, max_coalesce);
	if(credits_num == CREDITS_PER_REMOTE_WORKER){
		printf("INV_SS_GRANULARITY %d \t\t SEND_INV_Q_DEPTH %d \t\t RECV_INV_Q_DEPTH %d\n",
			   INV_SS_GRANULARITY, SEND_INV_Q_DEPTH, RECV_INV_Q_DEPTH);
		printf("ACK_SS_GRANULARITY %d \t\t SEND_ACK_Q_DEPTH %d \t\t RECV_ACK_Q_DEPTH %d\n",
			   ACK_SS_GRANULARITY, SEND_ACK_Q_DEPTH, RECV_ACK_Q_DEPTH);
		printf("VAL_SS_GRANULARITY %d \t\t SEND_VAL_Q_DEPTH %d \t\t RECV_VAL_Q_DEPTH %d\n",
			   VAL_SS_GRANULARITY, SEND_VAL_Q_DEPTH, RECV_VAL_Q_DEPTH);
		printf("CRD_SS_GRANULARITY %d \t\t SEND_CRD_Q_DEPTH %d \t\t RECV_CRD_Q_DEPTH %d\n",
			   CRD_SS_GRANULARITY, SEND_CRD_Q_DEPTH, RECV_CRD_Q_DEPTH);
	}

	thread_arr = malloc(num_workers * sizeof(pthread_t));
	param_arr  = malloc(num_workers * sizeof(struct thread_params));


	pthread_attr_t attr;
	cpu_set_t cpus_w;

	group_membership_init();
    init_stats((void*) w_stats);
	spacetime_init(machine_id, num_workers);


	pthread_attr_init(&attr);
	int w_core, init_core = SOCKET_TO_START_SPAWNING_THREADS;
	for(i = 0; i < num_workers; i++) {
		if(USE_ALL_SOCKETS && ENABLE_HYPERTHREADING)
			w_core = init_core + i;
		else
			w_core = 2 * i + init_core;
//        if(w_core > 19 ) w_core+=4;
		assert(ENABLE_HYPERTHREADING || w_core < TOTAL_NUMBER_OF_SOCKETS * TOTAL_CORES_PER_SOCKET);
		assert(w_core < TOTAL_HW_CORES);
		param_arr[i].id = i;

//		green_printf("Creating worker thread %d at core %d \n", param_arr[i].id, w_core);
		CPU_ZERO(&cpus_w);
		CPU_SET(w_core, &cpus_w);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus_w);
		pthread_create(&thread_arr[i], &attr, run_worker, &param_arr[i]);
	}

	yellow_printf("Sizes: {Op: %d, Object Meta %d, Value %d},\n",
				  sizeof(spacetime_op_t), sizeof(spacetime_object_meta), ST_VALUE_SIZE);

	yellow_printf("Coherence msg Sizes: {Inv: %d, Ack: %d, Val: %d, Crd: %d}\n",
				  sizeof(spacetime_inv_t), sizeof(spacetime_ack_t), sizeof(spacetime_val_t),
				  sizeof(spacetime_crd_t));

//	if(max_coalesce == MAX_REQ_COALESCE)
//		yellow_printf("Max Coalesce Packet Sizes: {Inv: %d, Ack: %d, Val: %d}\n",
//                      sizeof(wings_ud_send_pkt_t) + sizeof(spacetime_inv_t),
//                      sizeof(wings_ud_send_pkt_t) + sizeof(spacetime_ack_t),
//                      sizeof(wings_ud_send_pkt_t) + sizeof(spacetime_val_t));
////					  sizeof(spacetime_inv_packet_t), sizeof(spacetime_ack_packet_t),
////					  sizeof(spacetime_val_packet_t));
//	else
		yellow_printf("Max Coalesce Packet Sizes: {Inv: %d, Ack: %d, Val: %d}\n",
					  sizeof(wings_ud_send_pkt_t) + max_coalesce * sizeof(spacetime_inv_t),
					  sizeof(wings_ud_send_pkt_t) + max_coalesce * sizeof(spacetime_ack_t),
					  sizeof(wings_ud_send_pkt_t) + max_coalesce * sizeof(spacetime_val_t));

	for(i = 0; i < num_workers; i++)
		pthread_join(thread_arr[i], NULL);

	return 0;
}





//////////////////////////////////////////////////////////////////////////////////
/// Static asserts to ensure only correct configs
//////////////////////////////////////////////////////////////////////////////////

static_assert(MICA_MAX_VALUE >= ST_VALUE_SIZE, "");
static_assert(MACHINE_NUM <= 8, ""); //TODO haven't test bit vectors with more than 8 nodes
static_assert(MACHINE_NUM <= GROUP_MEMBERSHIP_ARRAY_SIZE * 8, "");//bit vector for acks / group membership
//static_assert(sizeof(spacetime_crd_t) < sizeof(((struct ibv_send_wr*)0)->imm_data), ""); //for inlined credits
static_assert(MACHINE_NUM <= 255, "");

//    static_assert(CREDITS_PER_REMOTE_WORKER <= MAX_BATCH_OPS_SIZE, "");

static_assert(MAX_PCIE_BCAST_BATCH <= INV_CREDITS, "");
static_assert(MAX_PCIE_BCAST_BATCH <= VAL_CREDITS, "");
static_assert(MAX_PCIE_BCAST_BATCH <= INV_SS_GRANULARITY, "");
static_assert(MAX_PCIE_BCAST_BATCH <= VAL_SS_GRANULARITY, "");

//	static_assert(SOCKET_TO_START_SPAWNING_THREADS < TOTAL_NUMBER_OF_SOCKETS, "");
static_assert((ENABLE_HYPERTHREADING == 1 && USE_ALL_SOCKETS == 1) || WORKERS_PER_MACHINE <= TOTAL_CORES_PER_SOCKET, "");
static_assert(WORKERS_PER_MACHINE <= TOTAL_HW_CORES, "");

// Increase in tail latency
// WARNING Increase of tail_latency [Currently is not working]
//static_assert(INCREASE_TAIL_LATENCY == 0 || NUM_OF_CORES_TO_INCREASE_TAIL <= WORKERS_PER_MACHINE, "");
//static_assert(!(INCREASE_TAIL_LATENCY && FAKE_FAILURE), "");

///Assertions for failures
static_assert(FAKE_FAILURE == 0 || NODE_TO_FAIL < MACHINE_NUM, "");
static_assert(FAKE_FAILURE == 0 || ROUNDS_BEFORE_FAILURE < PRINT_NUM_STATS_BEFORE_EXITING, "");
static_assert(FAKE_FAILURE == 0 || WORKER_WITH_FAILURE_DETECTOR < WORKERS_PER_MACHINE, "");

static_assert(MACHINE_NUM < TIE_BREAKER_ID_EMPTY, "");
static_assert(MACHINE_NUM < LAST_WRITER_ID_EMPTY, "");
static_assert(MAX_BATCH_OPS_SIZE < ST_OP_BUFFER_INDEX_EMPTY, ""); /// 1B write_buffer_index and 255 is used as "empty" value

///Make sure that assigned numbers to States are monotonically increasing with the following order
static_assert(VALID_STATE < INVALID_STATE, "");
static_assert(INVALID_STATE < INVALID_WRITE_STATE, "");
static_assert(INVALID_WRITE_STATE < WRITE_STATE, "");
static_assert(WRITE_STATE < REPLAY_STATE, "");

static_assert(MEASURE_LATENCY == 0 || THREAD_MEASURING_LATENCY < WORKERS_PER_MACHINE, "");

