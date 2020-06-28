/*
 * Copyright (C) 2019 Neil McGlohon
 * See LICENSE notice in top-level directory
 */

#include "codes/model-net.h"
#include "codes/lp-io.h"
#include "codes/codes.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/lp-type-lookup.h"
#include "kbarrays.h"
#include <float.h>


static int net_id = 0;
static int traffic = 1;
static double arrival_time = 1000.0;
static int PAYLOAD_SZ = 2048;
static int num_qos_levels = 1;
static int bw_reset_window = 1000;
static int max_qos_monitor = 50000;
static double max_peak_throughtput = 0.0;
static FILE* latency_file = NULL;

static int COLL_REPS = 1;
static int COLL_COUNT = 8;
static int coll_nodes[8] = {8,16,24,32,40,48,56,64};
//static int COLL_COUNT = 16;
//static int coll_nodes[16] = {53,57,6,37,4,38,39,65,4,13,42,25,49,41,15,4};

static int num_servers_per_rep = 0;
static int num_routers_per_grp = 0;
static int num_nodes_per_grp = 0;
static int num_nodes_per_router = 0;
static int num_groups = 0;
static unsigned long long num_nodes = 0;

//Dragonfly Custom Specific values
int num_router_rows, num_router_cols;

//Dragonfly Plus Specific Values
int num_router_leaf, num_router_spine;

//Dragonfly Dally Specific Values
int num_routers; //also used by original Dragonfly

static char lp_io_dir[256] = {'\0'};
static lp_io_handle io_handle;
static unsigned int lp_io_use_suffix = 0;
static int do_lp_io = 0;
static int num_msgs = 20;
static tw_stime sampling_interval = 800000;
static tw_stime sampling_end_time = 1600000;

typedef struct svr_msg svr_msg;
typedef struct svr_state svr_state;

/* global variables for codes mapping */
static char group_name[MAX_NAME_LENGTH];
static char lp_type_name[MAX_NAME_LENGTH];
static int group_index, lp_type_index, rep_id, offset;

/* statistic values for final output */
static tw_stime *max_global_server_latency;
static tw_stime *sum_global_server_latency;
static long long *sum_global_messages_received;
static tw_stime *mean_global_server_latency;

static long long *window_global_msgs_recvd;
static double *window_global_sum_latency;
static double *window_global_min_latency;
static double *window_global_max_latency;

/* type of events */
enum svr_event
{
    KICKOFF,	   /* kickoff event */
    REMOTE,        /* remote event */
    LOCAL,         /* local event */
    QOS_SNAP,   // KBEDIT
    QOS_SNAP_STATS, //KBEDIT
    TRANSFER_END
};

/* type of synthetic traffic */
enum TRAFFIC
{
	UNIFORM = 1, /* sends message to a randomly selected node */
    RAND_PERM = 2, 
	NEAREST_GROUP = 3, /* sends message to the node connected to the neighboring router */
	NEAREST_NEIGHBOR = 4, /* sends message to the next node (potentially connected to the same router) */
    RANDOM_OTHER_GROUP = 5,
    TARGETED = 9,        /* QoS test mini-workload */
    COLLECTIVE = 10       /* Have an all to all between select nodes */
};

struct svr_state
{
    int *msg_sent_count;   /* requests sent */
    int *msg_recvd_count;  /* requests recvd */
    int local_recvd_count; /* number of local messages received */
    tw_stime start_ts;    /* time that we started sending requests */
    tw_stime end_ts;      /* time that we ended sending requests */
    int svr_id;
    int dest_id;

    tw_stime *max_server_latency; /* maximum measured packet latency observed by server */
    tw_stime *sum_server_latency; /* running sum of measured latencies observed by server for calc of mean */

    int *transfers_completed_count;
    int *prev_msg_recvd_count;
    Array *my_times;
    int last_dest;
};

struct svr_msg
{
    enum svr_event svr_event_type;
    tw_lpid src;          /* source of this request or ack */
    tw_stime msg_start_time;
    int completed_sends; /* helper for reverse computation */
    tw_stime saved_time; /* helper for reverse computation */
    model_net_event_return event_rc;

    int qos_group; /* KBEDIT */
};

static void svr_init(
    svr_state * ns,
    tw_lp * lp);
static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp);
static void svr_finalize(
    svr_state * ns,
    tw_lp * lp);

tw_lptype svr_lp = {
    (init_f) svr_init,
    (pre_run_f) NULL,
    (event_f) svr_event,
    (revent_f) svr_rev_event,
    (commit_f) NULL,
    (final_f)  svr_finalize,
    (map_f) codes_mapping,
    sizeof(svr_state),
};

void dragonfly_svr_event_collect(svr_msg *m, tw_lp *lp, char *buffer, int *collect_flag)
{
    (void)lp;
    (void)collect_flag;
    int type = (int) m->svr_event_type;
    memcpy(buffer, &type, sizeof(type));
}

/* can add in any model level data to be collected along with simulation engine data
 * in the ROSS instrumentation.  Will need to update the last field in 
 * svr_model_types[0] for the size of the data to save in each function call
 */
void dragonfly_svr_model_stat_collect(svr_state *s, tw_lp *lp, char *buffer)
{
    (void)s;
    (void)lp;
    (void)buffer;
    return;
}

st_model_types dragonfly_svr_model_types[] = {
    {(ev_trace_f) dragonfly_svr_event_collect,
     sizeof(int),
     (model_stat_f) dragonfly_svr_model_stat_collect,
     0,
     NULL,
     NULL,
     0},
    {NULL, 0, NULL, 0, NULL, NULL, 0}
};

static const st_model_types  *dragonfly_svr_get_model_stat_types(void)
{
    return(&dragonfly_svr_model_types[0]);
}

void dragonfly_svr_register_model_types()
{
    st_model_type_register("nw-lp", dragonfly_svr_get_model_stat_types());
}

const tw_optdef app_opt [] =
{
        TWOPT_GROUP("Model net synthetic traffic " ),
    	TWOPT_UINT("traffic", traffic, "UNIFORM RANDOM=1, RANDOM PERM=2, NEAREST GROUP=3, NEAREST NEIGHBOR=4, RANDOM_OTHER_GROUP=5, TARGETED=9 "),
    	TWOPT_UINT("num_messages", num_msgs, "Number of messages to be generated per terminal "),
    	TWOPT_UINT("payload_sz",PAYLOAD_SZ, "size of the message being sent "),
    	TWOPT_STIME("sampling-interval", sampling_interval, "the sampling interval "),
    	TWOPT_STIME("sampling-end-time", sampling_end_time, "sampling end time "),
	    TWOPT_STIME("arrival_time", arrival_time, "INTER-ARRIVAL TIME"),
        TWOPT_CHAR("lp-io-dir", lp_io_dir, "Where to place io output (unspecified -> no output"),
        TWOPT_UINT("lp-io-use-suffix", lp_io_use_suffix, "Whether to append uniq suffix to lp-io directory (default 0)"),
        TWOPT_END()
};

const tw_lptype* svr_get_lp_type()
{
            return(&svr_lp);
}

static void svr_add_lp_type()
{
  lp_type_register("nw-lp", svr_get_lp_type());
}

static int is_collective_node(int nodeid)
{
    for(int i = 0; i < COLL_COUNT; i++)
    {
        if( nodeid == coll_nodes[i])
        {
            return 1;
        }
    }
    return 0;
}


static void issue_event(
    svr_state * ns,
    tw_lp * lp)
{
    (void)ns;
    tw_event *e;
    svr_msg *m;
    tw_stime kickoff_time;

    if(is_collective_node(ns->svr_id))
    {
        if(tw_now(lp) < 5)
        {
            for(int j = 1; j <= COLL_REPS; j++)
            {
                kickoff_time = 3000 + j*bw_reset_window + codes_local_latency(lp);
                for(int i = 0; i < COLL_COUNT; i++)
                {
                    kickoff_time += 10;
                    tw_event *e2 = tw_event_new(lp->gid, kickoff_time, lp);
                    svr_msg *m2 = tw_event_data(e2);
                    m2->svr_event_type = KICKOFF;
                    tw_event_send(e2);
                }
            }
        }
        return;

    }
    /* each server sends a dummy event to itself that will kick off the real
     * simulation
     */

    /* skew each kickoff event slightly to help avoid event ties later on */
    //kickoff_time = 1.1 * g_tw_lookahead + tw_rand_exponential(lp->rng, arrival_time);
    kickoff_time = arrival_time + codes_local_latency(lp);

    e = tw_event_new(lp->gid, kickoff_time, lp);
    m = tw_event_data(e);
    m->svr_event_type = KICKOFF;
    tw_event_send(e);
}

static void svr_init(
    svr_state * ns,
    tw_lp * lp)
{
    int i;
    ns->start_ts = 0.0;
    ns->dest_id = -1;
    ns->svr_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);
    
    ns->transfers_completed_count = (int*)calloc(num_qos_levels, sizeof(int));
    ns->max_server_latency = (tw_stime*)calloc(num_qos_levels, sizeof(tw_stime));
    ns->sum_server_latency = (tw_stime*)calloc(num_qos_levels, sizeof(tw_stime));
    ns->msg_recvd_count = (int*)calloc(num_qos_levels, sizeof(int));
    ns->msg_sent_count = (int*)calloc(num_qos_levels, sizeof(int));
    ns->prev_msg_recvd_count = (int*)calloc(num_qos_levels, sizeof(int));
    ns->my_times = (Array*)malloc(num_qos_levels * sizeof(Array));

    /* For collective nodes, set the last destination as myself */
    ns->last_dest = 0;
    for (i = 0; i < COLL_COUNT; i++)
    {
        if (coll_nodes[i] == ns->svr_id)
        {
            ns->last_dest = (i+1) % COLL_COUNT;
            break;
        }
    }

    /* If we're doing a synthetic qos run, print stats for reciever every window */
    //if (traffic == TARGETED && ns->svr_id == num_nodes_per_router){
    //if (ns->svr_id == num_nodes_per_router)  //num_nodes_per_router)
    //{
        //printf("### QOS recvd stats for node: %d\n", ns->svr_id);
        tw_event *e;
        svr_msg *m;
        e = tw_event_new(lp->gid, bw_reset_window, lp);
        m = tw_event_data(e);
        m->svr_event_type = QOS_SNAP;
        tw_event_send(e);
    //}
    for(i = 0; i < num_qos_levels; i++)
        initArray(&ns->my_times[i], num_msgs*1.05);

    /* If we're doing a synthetic qos run, only the first router of nodes should send messages */
    if (traffic == TARGETED && ns->svr_id >= 1) //num_nodes_per_router)
        return;
    if (traffic == NEAREST_GROUP && ns->svr_id >= num_nodes_per_grp)
        return;


    if (traffic == COLLECTIVE && is_collective_node(ns->svr_id))
    {
        printf("\n###QOS I'm a collective node! [%d]", ns->svr_id);
        //return;
    }

    if(ns->svr_id == 0)
    {
        //printf("### QOS_SNAPSHOT Time   Q1_msgs                        [throughput -  latency  ]        Q2_msgs [ throughput -  latency  ]");
        printf("### QOS                  (count)      (B/ns)       (ns)        (ns)        (ns)              (count)      (B/ns)       (ns)        (ns)        (ns)\n");
        printf("### QOS_SNAPSHOT Time    Q1_msgs [throughput -  avg_lat  |  min_lat  |  max_lat  ]           Q2_msgs [throughput -  avg_lat  |  min_lat  |  max_lat  ]\n");
    }
    //printf("Event issued on: %d\n", ns->svr_id);    // KBEDIT
    issue_event(ns, lp);
    return;
}

static void handle_qos_snap_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
}

static void handle_qos_snap_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    int i, j;
    for( i=0; i < num_qos_levels; i++)
    {
        max_global_server_latency[i] = 0;
        int window_count = ns->msg_recvd_count[i] - ns->prev_msg_recvd_count[i];
        window_global_msgs_recvd[i] += window_count;

        for (j = ns->prev_msg_recvd_count[i]; j < ns->msg_recvd_count[i]; j++)
        {
            window_global_sum_latency[i] += ns->my_times[i].record[j];

            if (max_global_server_latency[i] < ns->my_times[i].record[j])
                max_global_server_latency[i] = ns->my_times[i].record[j];
            if (window_global_max_latency[i] < ns->my_times[i].record[j])
                window_global_max_latency[i] = ns->my_times[i].record[j];
            if (window_global_min_latency[i] > ns->my_times[i].record[j])
                window_global_min_latency[i] = ns->my_times[i].record[j];
            //printf("Lat[%f], ", ns->my_times[i].record[j]);
        }

        if(window_count > 0)
        {
            fprintf(latency_file, "\n%.0lf %d %d ", tw_now(lp), ns->svr_id, i);
            printArray(latency_file, &ns->my_times[i], ns->prev_msg_recvd_count[i], ns->msg_recvd_count[i]);
        }
        ns->prev_msg_recvd_count[i] = ns->msg_recvd_count[i];
    }

    //printf("\n[%2d] I've injected: %d", ns->svr_id, ns->local_recvd_count);

    // Issue event to print the stats from this window
    //if(ns->svr_id == num_nodes_per_router) // KB to generalize
    if(ns->svr_id == 0) // KB to generalize
    {
        tw_event *e2;
        svr_msg *mg2;
        e2 = tw_event_new(lp->gid, 0.01 , lp);
        mg2 = tw_event_data(e2);
        mg2->svr_event_type = QOS_SNAP_STATS;
        tw_event_send(e2);
    }

    if(tw_now(lp) >= max_qos_monitor){
    //if(ns->msg_recvd_count[0] + ns->msg_recvd_count[1] >= num_msgs*4){
        //printf("\n\n###### Ending on node %d \n\n", ns->svr_id);
        return;
    }

    tw_event *e;
    svr_msg *mg;
    e = tw_event_new(lp->gid, bw_reset_window, lp);
    mg = tw_event_data(e);
    mg->svr_event_type = QOS_SNAP;
    tw_event_send(e);
    //printf("Event issued on: %d\n", ns->svr_id);    // KBEDIT
}
static void handle_qos_snap_stats_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
}

static void handle_qos_snap_stats_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    
    printf("### QOS_SNAPSHOT %-6.0lf  ", tw_now(lp));
    int i;
    float throughput, avg_latency;
    int payload_sz;
    if(is_collective_node(ns->svr_id))
    {
        payload_sz = PAYLOAD_SZ;
    }else{
        payload_sz = PAYLOAD_SZ;
    }

    for( i=0; i < num_qos_levels; i++)
    {
        throughput = PAYLOAD_SZ*window_global_msgs_recvd[i]/bw_reset_window;
        avg_latency = window_global_sum_latency[i]/window_global_msgs_recvd[i];
        
        if (window_global_msgs_recvd[i] == 0)
        {
            avg_latency = 0.0;
            window_global_min_latency[i] = 0.0;
        }
        
        printf("%-7lld [ %8.2f  - %8.2f  | %8.2f  | %8.2f  ]           ", window_global_msgs_recvd[i], 
                throughput, avg_latency, window_global_min_latency[i], window_global_max_latency[i]);
        window_global_msgs_recvd[i] = 0;
        window_global_sum_latency[i] = 0.0;
        window_global_min_latency[i] = DBL_MAX;
        window_global_max_latency[i] = 0.0;
    }
    printf("\n");
}

static void handle_transfer_end_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    ns->transfers_completed_count[m->qos_group]--;
}

static void handle_transfer_end_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    ns->transfers_completed_count[m->qos_group]++;
}

static void handle_kickoff_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
    if(m->completed_sends)
        return;

    if(b->c1)
        tw_rand_reverse_unif(lp->rng);

    if(b->c8)
        tw_rand_reverse_unif(lp->rng);
    if(traffic == RANDOM_OTHER_GROUP) {
        tw_rand_reverse_unif(lp->rng);
        tw_rand_reverse_unif(lp->rng);
    }

    model_net_event_rc2(lp, &m->event_rc);
    ns->msg_sent_count[0];  // KB Correctly handle RC
    tw_rand_reverse_unif(lp->rng);
}
static void handle_kickoff_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
    int i;
    for (i = 0; i < num_qos_levels; i++)
    {
        if(ns->msg_sent_count[i] >= num_msgs)
        {
            if(traffic == TARGETED)
            {
                //printf("[%d] Ending at: %lf\n", ns->svr_id, tw_now(lp));
            }
            m->completed_sends = 1;
            return;
        }
    }

    m->completed_sends = 0;

    char anno[MAX_NAME_LENGTH];
    tw_lpid local_dest = -1, global_dest = -1;

    svr_msg * m_local = malloc(sizeof(svr_msg));
    svr_msg * m_remote = malloc(sizeof(svr_msg));

    m_local->svr_event_type = LOCAL;
    m_local->src = lp->gid;
    m_local->msg_start_time = tw_now(lp);

    memcpy(m_remote, m_local, sizeof(svr_msg));
    m_remote->svr_event_type = REMOTE;

    ns->start_ts = tw_now(lp);
    codes_mapping_get_lp_info(lp->gid, group_name, &group_index, lp_type_name, &lp_type_index, anno, &rep_id, &offset);
    int local_id = codes_mapping_get_lp_relative_id(lp->gid, 0, 0);

   /* in case of uniform random traffic, send to a random destination. */
   if(traffic == UNIFORM)
   {
    b->c1 = 1;
    local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
    local_dest = (ns->svr_id + local_dest) % num_nodes;
        while(is_collective_node(local_dest))
        {
            local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
            local_dest = (ns->svr_id + local_dest) % num_nodes;
        }
   }
   else if(traffic == NEAREST_GROUP)
   {
	local_dest = (local_id + num_nodes_per_grp) % num_nodes;
	//printf("\n LP %ld sending to %ld num nodes %d ", local_id, local_dest, num_nodes);
   }
   else if(traffic == NEAREST_NEIGHBOR)
   {
	local_dest =  (local_id + 1) % num_nodes;
//	 printf("\n LP %ld sending to %ld num nodes %d ", rep_id * 2 + offset, local_dest, num_nodes);
   }
   else if(traffic == RAND_PERM)
   {
       if(ns->dest_id == -1)
       {
            b->c8 = 1;
            ns->dest_id = tw_rand_integer(lp->rng, 0, num_nodes - 1); 
            local_dest = ns->dest_id;
       }
       else
       {
        local_dest = ns->dest_id; 
       }
   }
   else if(traffic == RANDOM_OTHER_GROUP)
   {
       int my_group_id = local_id / num_nodes_per_grp;

       int other_groups[num_groups-1];
       int added =0;
       for(int i = 0; i < num_groups; i++)
       {
           if(i != my_group_id) {
               other_groups[added] = i;
               added++;
           }
       }
        int rand_group = other_groups[tw_rand_integer(lp->rng,0,added -1)];
        int rand_node_intra_id = tw_rand_integer(lp->rng, 0, num_nodes_per_grp-1);

        local_dest = (rand_group * num_nodes_per_grp) + rand_node_intra_id;
        printf("\n LP %d sending to %llu num nodes %llu ", local_id, LLU(local_dest), num_nodes);

   }
   else if(traffic == TARGETED){
        local_dest = num_nodes_per_router; // Sends traffic to the first node in the second router
        //local_dest = num_nodes_per_grp; // Sends traffic to the first node in the second router
        //local_dest = tw_rand_integer(lp->rng, num_nodes_per_router, num_nodes_per_router*2 - 1); // send traffic to a random node on the second router
    }
    else if(traffic == COLLECTIVE){
        if(!is_collective_node(ns->svr_id)){
            local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
            local_dest = (ns->svr_id + local_dest) % num_nodes;
            while(is_collective_node(local_dest))
            {
                local_dest = tw_rand_integer(lp->rng, 1, num_nodes - 2);
                local_dest = (ns->svr_id + local_dest) % num_nodes;
            }
        }else{
            int next_node= (ns->last_dest +1) % COLL_COUNT;
            local_dest = coll_nodes[next_node];
            if (local_dest == ns->svr_id) // Don't send to self
            {
                ns->last_dest = (ns->last_dest +1) % COLL_COUNT;
                return;
            }
            ns->last_dest = next_node;
        }
    }
    assert(local_dest < num_nodes);
//   codes_mapping_get_lp_id(group_name, lp_type_name, anno, 1, local_dest / num_servers_per_rep, local_dest % num_servers_per_rep, &global_dest);
    global_dest = codes_mapping_get_lpid_from_relative(local_dest, group_name, lp_type_name, NULL, 0);
    if (is_collective_node(ns->svr_id))  // to be generalized
    {
        ns->msg_sent_count[0]++;
        m_remote->qos_group = 0;
        m->event_rc = model_net_event(net_id, "high", global_dest, PAYLOAD_SZ , 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
        return;
    }else
    {
        ns->msg_sent_count[1]++;
        m_remote->qos_group = 1;
        m->event_rc = model_net_event(net_id, "medium", global_dest, PAYLOAD_SZ, 0.0, sizeof(svr_msg), (const void*)m_remote, sizeof(svr_msg), (const void*)m_local, lp);
    }
//   if (ns->msg_recvd_count[0] + ns->msg_recvd_count[1] < 100)  // KBEDIT
    issue_event(ns, lp);
    return;
}

static void handle_remote_rev_event(
            svr_state * ns,
            tw_bf * b,
            svr_msg * m,
            tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
        ns->msg_recvd_count[b->c2]--;

        tw_stime packet_latency = tw_now(lp) - m->msg_start_time;
        ns->sum_server_latency[0] -= packet_latency;
        if (b->c2) // KB to be fixed
            ns->max_server_latency[0] = m->saved_time;

}

static void handle_remote_event(
	    svr_state * ns,
	    tw_bf * b,
	    svr_msg * m,
	    tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
    // KBEDIT

//    if (tw_now(lp) < 2500 || tw_now(lp) > 5000)
//        return;
    const int qos_level = m->qos_group;
    b->c2 = qos_level;

    tw_stime msg_latency = tw_now(lp) - m->msg_start_time;
    
    ns->msg_recvd_count[qos_level]++;
    ns->sum_server_latency[qos_level] += msg_latency;
    if (msg_latency > ns->max_server_latency[qos_level]) {
        m->saved_time = ns->max_server_latency[qos_level];
        ns->max_server_latency[qos_level] = msg_latency;
    }

    insertArray(&ns->my_times[qos_level], ns->msg_recvd_count[qos_level] -1, msg_latency);
    //if (ns->svr_id == 1)
    //    printf("0000 - from %d\n", codes_mapping_get_lp_relative_id(m->src, 0, 0));

    /* Notify sender that the transfer is complete */
    tw_event *e;
    svr_msg *mg;
    e = tw_event_new(m->src, 1.05, lp);
    mg = tw_event_data(e);
    mg->qos_group = m->qos_group;
    mg->svr_event_type = TRANSFER_END;
    tw_event_send(e);
}

static void handle_local_rev_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
	ns->local_recvd_count--;
}

static void handle_local_event(
                svr_state * ns,
                tw_bf * b,
                svr_msg * m,
                tw_lp * lp)
{
        (void)b;
        (void)m;
        (void)lp;
    ns->local_recvd_count++;
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}

static void svr_finalize(
    svr_state * ns,
    tw_lp * lp)
{
    ns->end_ts = tw_now(lp);
    //if (ns->svr_id ==0) printf("Finalizing now at: %lf\n", tw_now(lp));

    int i;
    for (i = 0; i < num_qos_levels; i++){   // KBEDIT

        //add to the global running sums
        sum_global_server_latency[i] += ns->sum_server_latency[i];
        sum_global_messages_received[i] += ns->msg_recvd_count[i];
        //compare to global maximum
        if (ns->max_server_latency[i] > max_global_server_latency[i])
            max_global_server_latency[i] = ns->max_server_latency[i];
    }
    //this server's mean
    // tw_stime mean_packet_latency = ns->sum_server_latency/ns->msg_recvd_count;


    //printf("server %llu recvd %d bytes in %f seconds, %f MiB/s sent_count %d recvd_count %d local_count %d \n", (unsigned long long)lp->gid, PAYLOAD_SZ*ns->msg_recvd_count, ns_to_s(ns->end_ts-ns->start_ts),
    //    ((double)(PAYLOAD_SZ*ns->msg_sent_count)/(double)(1024*1024)/ns_to_s(ns->end_ts-ns->start_ts)), ns->msg_sent_count, ns->msg_recvd_count, ns->local_recvd_count);
    return;
}

static void svr_rev_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
    switch (m->svr_event_type)
    {
	case REMOTE:
		handle_remote_rev_event(ns, b, m, lp);
		break;
	case LOCAL:
		handle_local_rev_event(ns, b, m, lp);
		break;
	case KICKOFF:
		handle_kickoff_rev_event(ns, b, m, lp);
		break;
        case QOS_SNAP:
                handle_qos_snap_rev_event(ns, b, m, lp);
                break;
        case QOS_SNAP_STATS:
                handle_qos_snap_stats_rev_event(ns, b, m, lp);
                break;
        case TRANSFER_END:
                handle_transfer_end_rev_event(ns, b, m, lp);
                break;
	default:
		assert(0);
		break;
    }
}

static void svr_event(
    svr_state * ns,
    tw_bf * b,
    svr_msg * m,
    tw_lp * lp)
{
   switch (m->svr_event_type)
    {
        case REMOTE:
            handle_remote_event(ns, b, m, lp);
            break;
        case LOCAL:
            handle_local_event(ns, b, m, lp);
            break;
	case KICKOFF:
	    handle_kickoff_event(ns, b, m, lp);
	    break;
        case QOS_SNAP:
                handle_qos_snap_event(ns, b, m, lp);
                break;
        case QOS_SNAP_STATS:
                handle_qos_snap_stats_event(ns, b, m, lp);
                break;
        case TRANSFER_END:
                handle_transfer_end_event(ns, b, m, lp);
                break;
        default:
            printf("\n Invalid message type %d ", m->svr_event_type);
            assert(0);
        break;
    }
}

static void init_global_stats()
{
    sum_global_messages_received = (long long int*)calloc(num_qos_levels, sizeof(long long int));
    max_global_server_latency = (tw_stime*) calloc(num_qos_levels, sizeof(tw_stime));
    sum_global_server_latency = (tw_stime*) calloc(num_qos_levels, sizeof(tw_stime));
    mean_global_server_latency = (tw_stime*) calloc(num_qos_levels, sizeof(tw_stime));

    window_global_msgs_recvd = (long long int*)calloc(num_qos_levels, sizeof(long long int));
    window_global_sum_latency = (double*)calloc(num_qos_levels, sizeof(double));
    window_global_min_latency = (double*)calloc(num_qos_levels, sizeof(double));
    window_global_max_latency = (double*)calloc(num_qos_levels, sizeof(double));

    for(int i=0; i < num_qos_levels; i++)
        window_global_min_latency[i] = DBL_MAX;
}

// does MPI reduces across PEs to generate stats based on the global static variables in this file
static void svr_report_stats()
{
    long long total_received_messages;
    tw_stime total_sum_latency, max_latency, mean_latency;
    
    int i;
    for(i = 0; i < num_qos_levels; i++){
        MPI_Reduce( &sum_global_messages_received[i], &total_received_messages, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_CODES);
        MPI_Reduce( &sum_global_server_latency[i], &total_sum_latency, 1,MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_CODES);
        MPI_Reduce( &max_global_server_latency[i], &max_latency, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_CODES);
    
        mean_latency = total_sum_latency / total_received_messages;
        if(!g_tw_mynode)
        {	
            printf("\nSynthetic Workload LP Stats [QOS-class-%d]: Mean Message Latency: %lf us,  Maximum Message Latency: %lf us,  Total Messages Received: %lld",
                    i, (float)mean_latency / 1000, (float)max_latency / 1000, total_received_messages);
        }
    }
    printf("\n");
}


int main(
    int argc,
    char **argv)
{
    int nprocs;
    int rank;
    int num_nets;
    int *net_ids;

    tw_opt_add(app_opt);
    tw_init(&argc, &argv);

#ifdef USE_RDAMARIS
    if(g_st_ross_rank)
    { // keep damaris ranks from running code between here up until tw_end()
#endif
    codes_comm_update();


    if(argc < 2)
    {
            printf("\n Usage: mpirun <args> --sync=1/2/3 -- <config_file.conf> ");
            MPI_Finalize();
            return 0;
    }

    MPI_Comm_rank(MPI_COMM_CODES, &rank);
    MPI_Comm_size(MPI_COMM_CODES, &nprocs);

    printf("\n\n\n ============ This simulaiton work in serial sequential mode ONLY !!!! ========= \n\n\n");

    configuration_load(argv[2], MPI_COMM_CODES, &config);

    model_net_register();
    svr_add_lp_type();

    if (g_st_ev_trace || g_st_model_stats || g_st_use_analysis_lps)
        dragonfly_svr_register_model_types();

    codes_mapping_setup();

    net_ids = model_net_configure(&num_nets);
    //assert(num_nets==1);
    net_id = *net_ids;
    free(net_ids);

    latency_file = fopen("r_latencies.txt", "w");
    fprintf(latency_file, "snapshot_ts svr_id qos_id latency,[latency,]...");

    /* 5 days of simulation time */
    g_tw_ts_end = s_to_ns(5 * 24 * 60 * 60);
    //g_tw_ts_end = s_to_ns(0.00001);
    model_net_enable_sampling(sampling_interval, sampling_end_time);

    if(!(net_id == DRAGONFLY_DALLY || net_id == DRAGONFLY_PLUS || net_id == DRAGONFLY_CUSTOM || net_id == DRAGONFLY))
    {
	printf("\n The workload generator is designed to only work with Dragonfly based model (Dally, Plus, Custom, Original) configuration only! %d %d ", DRAGONFLY_DALLY, net_id);
        MPI_Finalize();
        return 0;
    }
    num_servers_per_rep = codes_mapping_get_lp_count("MODELNET_GRP", 1, "nw-lp",
            NULL, 1);

    int num_routers_with_cns_per_group;

    if (net_id == DRAGONFLY_DALLY) {
        int global_links_per_router;
        double global_bandwidth;
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Dally\n");
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        num_routers_with_cns_per_group = num_routers;

        configuration_get_value_double(&config, "PARAMS", "global_bandwidth", NULL, &global_bandwidth);
        configuration_get_value_int(&config, "PARAMS", "num_global_channels", NULL, &global_links_per_router);
        max_peak_throughtput = global_links_per_router * global_bandwidth * num_routers * num_groups;
    }
    else if (net_id == DRAGONFLY_PLUS) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Plus\n");
        configuration_get_value_int(&config, "PARAMS", "num_router_leaf", NULL, &num_router_leaf);
        configuration_get_value_int(&config, "PARAMS", "num_router_spine", NULL, &num_router_spine);
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        num_routers_with_cns_per_group = num_router_leaf;

    }
    else if (net_id == DRAGONFLY_CUSTOM) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Custom\n");
        configuration_get_value_int(&config, "PARAMS", "num_router_rows", NULL, &num_router_rows);
        configuration_get_value_int(&config, "PARAMS", "num_router_cols", NULL, &num_router_cols);
        configuration_get_value_int(&config, "PARAMS", "num_groups", NULL, &num_groups);
        configuration_get_value_int(&config, "PARAMS", "num_cns_per_router", NULL, &num_nodes_per_router);
        num_routers_with_cns_per_group = num_router_rows * num_router_cols;
    }
    else if (net_id == DRAGONFLY) {
        if (!rank)
            printf("Synthetic Generator: Detected Dragonfly Original 1D\n");
        configuration_get_value_int(&config, "PARAMS", "num_routers", NULL, &num_routers);
        num_nodes_per_router = num_routers/2;
        num_routers_with_cns_per_group = num_routers;
        num_groups = num_routers * num_nodes_per_router + 1;
    }

    num_nodes = num_groups * num_routers_with_cns_per_group * num_nodes_per_router;
    num_nodes_per_grp = num_routers_with_cns_per_group * num_nodes_per_router;

    assert(num_nodes);

    if(lp_io_dir[0])
    {
        do_lp_io = 1;
        int flags = lp_io_use_suffix ? LP_IO_UNIQ_SUFFIX : 0;
        int ret = lp_io_prepare(lp_io_dir, flags, &io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_prepare failure");
    }
    int rc;
    rc = configuration_get_value_int(&config, "PARAMS", "num_qos_levels", NULL, &num_qos_levels);
    if(!rc){
        configuration_get_value_int(&config, "PARAMS", "bw_reset_window", NULL, &bw_reset_window);
        configuration_get_value_int(&config, "PARAMS", "max_qos_monitor", NULL, &max_qos_monitor);
        printf("\n### QOS Num active classes:  %d", num_qos_levels);
        printf("\n### QOS BW reset window:     %dns", bw_reset_window);
        printf("\n### QOS Monitoring ends:     %dns", max_qos_monitor);
        printf("\n### QOS Global theoretical peak throughput: %f GiB/s", max_peak_throughtput);
        printf("\n### QOS\n");
    }

    init_global_stats();

    tw_run();
    if (do_lp_io){
        int ret = lp_io_flush(io_handle, MPI_COMM_CODES);
        assert(ret == 0 || !"lp_io_flush failure");
    }
    model_net_report_stats(net_id);
    svr_report_stats();
#ifdef USE_RDAMARIS
    } // end if(g_st_ross_rank)
#endif
    tw_end();

    fclose(latency_file);
    return 0;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
