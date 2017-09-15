/*
   Copyright 2016 Vanderbilt University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#define _log_module_index 126

#include <apr_errno.h>
#include <apr_pools.h>
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>
#include <apr_thread_proc.h>
#include <apr_time.h>
#include <gop/portal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <tbx/apr_wrapper.h>
#include <tbx/atomic_counter.h>
#include <tbx/fmttypes.h>
#include <tbx/log.h>
#include <tbx/network.h>
#include <tbx/stack.h>
#include <tbx/type_malloc.h>

#include "gop.h"
#include "gop/hp.h"
#include "gop/types.h"
#include "host_portal.h"

//*************************************************************************
// new_host_connection - Allocates space for a new connection
//*************************************************************************

gop_host_connection_t *new_host_connection(apr_pool_t *mpool)
{
    gop_host_connection_t *hc;

    tbx_type_malloc_clear(hc, gop_host_connection_t, 1);

    hc->mpool = mpool;
    apr_thread_mutex_create(&(hc->lock), APR_THREAD_MUTEX_DEFAULT, mpool);
    apr_thread_cond_create(&(hc->send_cond), mpool);
    apr_thread_cond_create(&(hc->recv_cond), mpool);
    hc->pending_stack = tbx_stack_new();
    hc->cmd_count = 0;
    hc->curr_workload = 0;
    hc->shutdown_request = 0;
    hc->ns = tbx_ns_new();
    hc->hp = NULL;
    hc->curr_op = NULL;
    hc->last_used = 0;

    log_printf(15, "ns=%d\n", tbx_ns_getid(hc->ns));
    return(hc);
}

//*************************************************************************
// destroy_host_connection - Frees space allocated to a depot connection
//*************************************************************************

void destroy_host_connection(gop_host_connection_t *hc)
{
    log_printf(15, "host=%s ns=%d closing=%d\n", hc->hp->host, tbx_ns_getid(hc->ns), hc->closing);
    tbx_ns_destroy(hc->ns);
    tbx_stack_free(hc->pending_stack, 0);
    apr_thread_mutex_destroy(hc->lock);
    apr_thread_cond_destroy(hc->send_cond);
    apr_thread_cond_destroy(hc->recv_cond);
    apr_pool_destroy(hc->mpool);
    free(hc);
}

//*************************************************************************
// close_hc - Closes a depot connection
//*************************************************************************

void close_hc(gop_host_connection_t *hc, int quick)
{
    apr_status_t value;
    gop_host_portal_t *hp;

    //** Trigger the send thread to shutdown which also closes the recv thread
    log_printf(15, "close_hc: Closing ns=%d closing=%d quick=%d\n", tbx_ns_getid(hc->ns), hc->closing, quick);
    lock_hc(hc);
    hc->shutdown_request = 1;
    unlock_hc(hc);

    //** There are 2 types of waits: 1)empty hportal que 2)local que full
    //**(1) This wakes up everybody on the depot:(
    hportal_lock(hc->hp);
    hportal_signal(hc->hp);
    hportal_unlock(hc->hp);
    //**(2) local que is full
    lock_hc(hc);
    hc_send_signal(hc);
    unlock_hc(hc);
    hportal_lock(hc->hp);
    hportal_signal(hc->hp);
    hportal_unlock(hc->hp);

    if (quick == 1) {  //** Quick shutdown.  Don't wait and clean up.
        lock_hc(hc);
        hc->closing = 2;  //** Flag a reaper that I'm done with it and not to join
        unlock_hc(hc);
        return;
    }

    //** Wait until the recv thread completes
    apr_thread_join(&value, hc->recv_thread);

    //** Let the reaper take it but don't join since I've already done it.
    lock_hc(hc);
    hc->closing = 3;
    unlock_hc(hc);

    hp = hc->hp;
    hportal_lock(hp);
    _reap_hportal(hp, quick);  //** Clean up the closed connections.  Including hc passed in
    hportal_unlock(hp);
}

//*************************************************************
// check_workload - Waits until the workload is acceptable
//   before continuing.  It returns the size of the pending stack
//*************************************************************

int check_workload(gop_host_connection_t *hc)
{
    int psize;
    apr_time_t dt;
    int sec;

    lock_hc(hc);
    while ((hc->curr_workload >= hc->hp->context->max_workload) && (hc->shutdown_request == 0)) {
        dt = apr_time_now();
        sec = dt / APR_USEC_PER_SEC;
        log_printf(15, "check_workload: *workload loop* shutdown_request=%d stack_size=%d curr_workload=%d time=%d sec\n", hc->shutdown_request, tbx_stack_count(hc->pending_stack), hc->curr_workload, sec);
        apr_thread_cond_wait(hc->send_cond, hc->lock);
        dt = apr_time_now() - dt;
        sec = dt / APR_USEC_PER_SEC;
        log_printf(15, "check_workload: *workload loop* AFTER sleep shutdown_request=%d stack_size=%d curr_workload=%d slept=%d sec\n", hc->shutdown_request, tbx_stack_count(hc->pending_stack), hc->curr_workload, sec);
    }

    psize = tbx_stack_count(hc->pending_stack);
    unlock_hc(hc);

    return(psize);
}

//*************************************************************
// empty_work_que - Pauses until all pending commands have
//   completed processing.
//*************************************************************

void empty_work_que(gop_host_connection_t *hc)
{
    lock_hc(hc);
    while (tbx_stack_count(hc->pending_stack) != 0) {
        log_printf(15, "empty_work_que: shutdown_request=%d stack_size=%d curr_workload=%d\n", hc->shutdown_request, tbx_stack_count(hc->pending_stack), hc->curr_workload);
        apr_thread_cond_signal(hc->recv_cond);
        apr_thread_cond_wait(hc->send_cond, hc->lock);
    }
    unlock_hc(hc);
}

//*************************************************************
// empty_hp_que - Empties the work que.  Failing all tasks
//*************************************************************

void empty_hp_que(gop_host_portal_t *hp, gop_op_status_t err_code)
{
    hportal_lock(hp);
    _hp_fail_tasks(hp, err_code);
    hportal_unlock(hp);
}

//*************************************************************
// wait_for_work - Pauses the recv thread until new work is
//   available or finished.
//*************************************************************

void recv_wait_for_work(gop_host_connection_t *hc)
{
    lock_hc(hc);
    while ((hc->shutdown_request == 0) && (tbx_stack_count(hc->pending_stack) == 0)) {
        hc_send_signal(hc);
        log_printf(5, "shutdown_request=%d\n", hc->shutdown_request);
        apr_thread_cond_wait(hc->recv_cond, hc->lock);
    }
    unlock_hc(hc);
}

//*************************************************************
// hc_send_thread - Handles the sending phase of a command
//*************************************************************

void *hc_send_thread(apr_thread_t *th, void *data)
{
    gop_host_connection_t *hc = (gop_host_connection_t *)data;
    gop_host_portal_t *hp = hc->hp;
    tbx_ns_t *ns = hc->ns;
    gop_portal_context_t *hpc = hp->context;
    gop_command_op_t *hop;
    gop_op_generic_t *hsop;
    gop_op_status_t finished;
    tbx_ns_timeout_t dt;
    apr_time_t dtime;
    int tid, err, idle_state;

    hportal_lock(hp);
    hp->oops_send_start++;
    hportal_unlock(hp);

    //** Wait until the recv thread has started.
    lock_hc(hc);
    while (hc->recv_up == 0) {
        apr_thread_cond_wait(hc->send_cond, hc->lock);
    }
    err = hc->recv_up;
    unlock_hc(hc);

    log_printf(5, "hc->recv_up=%d host=%s\n",err, hp->host);

    idle_state = 0;

    if (err != 1) return(NULL);  //** If send thread failed to spawn exit;

    //** check if the host is invalid and if so flush the work que
    if (hp->invalid_host == 1) {
        log_printf(15, "hc_send_thread: Invalid host to host=%s:%d.  Emptying Que\n", hp->host, hp->port);
        empty_hp_que(hp, op_invalid_host_status);
        hc->net_connect_status = 1;
    } else {  //** Make the connection
        hc->net_connect_status = hpc->fn->connect(ns, hp->connect_context, hp->host, hp->port, hp->dt_connect);
        if (hc->net_connect_status != 0) {
            log_printf(5, "hc_send_thread:  Can't connect to %s:%d!, ns=%d\n", hp->host, hp->port, tbx_ns_getid(ns));
        }
    }

    tid = tbx_atomic_thread_id;
    log_printf(2, "hc_send_thread: New connection to host=%s:%d ns=%d tid=%d\n", hp->host, hp->port, tbx_ns_getid(ns), tid);


    //** Store my position in the conn_list **
    hportal_lock(hp);
    hc->start_stable = hp->stable_conn;

    if (hc->net_connect_status == 0) {
        hp->successful_conn_attempts++;
        hp->failed_conn_attempts = 0;  //** Reset the failed attempts
    } else {
        log_printf(1, "hc_send_thread: ns=%d failing all commands failed_conn_attempts=%d\n", tbx_ns_getid(ns), hp->failed_conn_attempts);
        hp->failed_conn_attempts++;
    }
    tbx_stack_push(hp->conn_list, (void *)hc);
    hc->my_pos = tbx_stack_get_current_ptr(hp->conn_list);
    hportal_unlock(hp);

    //** Now we start the main loop
    hsop = NULL;
    hop = NULL;
    finished = gop_success_status;
    tbx_ns_timeout_set(&dt, 1, 0);

    if (hc->net_connect_status != 0) finished = op_dead_status;  //** If connect() failed err out
    while (finished.op_status == OP_STATE_SUCCESS) {
        //** Wait for the load to go down if needed **
        check_workload(hc);

        //** Now get the next command **
        hportal_lock(hp);

        hsop = _get_hportal_op(hp);
        if (hsop == NULL) {
            if (idle_state == 0) {  // ** Flag us as idle if needed
                idle_state = 1;
                tbx_atomic_inc(hp->idle_conn);
            }
            log_printf(15, "hc_send_thread: No commands so sleeping.. ns=%d time=" TT "\n", tbx_ns_getid(ns), apr_time_now());
            hportal_wait(hp, 1);    //** Wait for a new task
            hportal_unlock(hp);
        } else { //** Got one so let's process it
            if (idle_state == 1) {
                idle_state = 0;
                tbx_atomic_dec(hp->idle_conn);
            }

            hop = &(hsop->op->cmd);
            hportal_unlock(hp);

            log_printf(5, "hc_send_thread: Processing new command.. ns=%d gid=%d\n", tbx_ns_getid(ns), gop_id(hsop));

            hop->start_time = apr_time_now();  //** This is changed in the recv phase also
            hop->end_time = hop->start_time + hop->timeout;

            lock_hc(hc);  //** Check if this command is on top
            hc->curr_op = hsop;  //** Make sure the current op doesn't get lost if needed
            if (tbx_stack_count(hc->pending_stack) == 0) tbx_atomic_set(hop->on_top, 1);
            unlock_hc(hc);

            finished = (hop->send_command != NULL) ? hop->send_command(hsop, ns) : gop_success_status;
            log_printf(5, "hc_send_thread: after send command.. ns=%d gid=%d finisehd=%d\n", tbx_ns_getid(ns), gop_id(hsop), finished.op_status);
            if (finished.op_status == OP_STATE_SUCCESS) {
                lock_hc(hc);
                hc->last_used = apr_time_now();  //** Update  the time.  The recv thread does this also
                hc->curr_workload += hop->workload;  //** Inc the current workload
                if (tbx_atomic_get(hop->on_top) == 0) {
                    if (tbx_stack_count(hc->pending_stack) == 0) {
                        tbx_atomic_set(hop->on_top, 1);
                        hop->start_time = apr_time_now();  //** This is the real start/end time now
                        hop->end_time = hop->start_time + hop->timeout;
                    }
                }
                unlock_hc(hc);

                log_printf(15, "hc_send_thread: before send phase.. ns=%d gid=%d\n", tbx_ns_getid(ns), gop_id(hsop));
                finished = (hop->send_phase != NULL) ? hop->send_phase(hsop, ns) : gop_success_status;
                log_printf(5, "hc_send_thread: after send phase.. ns=%d gid=%d finisehd=%d\n", tbx_ns_getid(ns), gop_id(hsop), finished.op_status);

                //** Always push the command on the recving que even in a failure to collect the return code
                lock_hc(hc);
                hc->last_used = apr_time_now();  //** Update  the time.  The recv thread does this also
                tbx_stack_push(hc->pending_stack, (void *)hsop);  //** Push onto recving stack
                hc->curr_op = NULL;
                hc_recv_signal(hc); //** and notify recv thread
                unlock_hc(hc);
                hsop = NULL;  //** It's on the stack so don't accidentally add it twice if there's a problem
                hop = NULL;
            }
        }

        lock_hc(hc);

        if (tbx_stack_count(hc->pending_stack) == 0) {
            dtime = apr_time_now() - hc->last_used; //** Exit if not busy
            if (dtime >= hpc->min_idle) {
                hc->shutdown_request = 1;
                log_printf(5, "hc_send_thread: ns=%d min_idle(" TT ") reached.  Shutting down! dtime=" TT "\n",
                           tbx_ns_getid(ns), hpc->min_idle, dtime);
            }
        } else if (hc->start_stable == 0) {
            log_printf(5, "hc_send_thread: ns=%d start_stable=0 using non-persistent sockets Shutting down!\n", tbx_ns_getid(ns));
            hc->shutdown_request=1;
        }

        if (hc->shutdown_request == 1) {
            finished = gop_error_status;
            log_printf(5, "hc_send_thread: ns=%d shutdown request!\n", tbx_ns_getid(ns));
        }

        log_printf(15, "hc_send_thread: ns=%d shutdown=%d stack_size=%d curr_workload=%d time=" TT " last_used=" TT "\n", tbx_ns_getid(ns),
                   hc->shutdown_request, tbx_stack_count(hc->pending_stack), hc->curr_workload, apr_time_now(), hc->last_used);
        unlock_hc(hc);
    }


    //** Cleanup the idle satus if needed
    if (idle_state == 1) {
        idle_state = 0;
        tbx_atomic_dec(hp->idle_conn);
    }

    //** Make sure and trigger the recv if their was a problem **
    lock_hc(hc);

    log_printf(15, "hc_send_thread: Exiting! (ns=%d, host=%s:%d)\n", tbx_ns_getid(ns), hp->host, hp->port);

    hc->shutdown_request = (tbx_stack_count(hc->pending_stack) == 0) ? 1 : 2;
    apr_thread_cond_signal(hc->recv_cond);
    unlock_hc(hc);

    //*** The recv side handles the removal from the hportal structure ***
    modify_hpc_thread_count(hpc, -1);

    lock_hc(hc);  //** Notify anybody listening that the send side is down.
    hc->send_down = 1;
    apr_thread_cond_signal(hc->send_cond);
    unlock_hc(hc);

    hportal_lock(hp);
    hp->oops_send_end++;
    hportal_unlock(hp);

    apr_thread_exit(th, 0);
    return(NULL);
}

//*************************************************************
// hc_recv_thread - Handles the recving phase of a command
//*************************************************************

void *hc_recv_thread(apr_thread_t *th, void *data)
{
    gop_host_connection_t *hc = (gop_host_connection_t *)data;
    tbx_ns_t *ns = hc->ns;
    gop_host_portal_t *hp = hc->hp;
    gop_portal_context_t *hpc = hp->context;
    apr_status_t value;
    apr_time_t cmd_pause_time = 0;
    apr_time_t pause_until;
    int64_t start_cmds_processed, cmds_processed;
    apr_time_t check_time;
    int finished, pending, n, tid, firsttime;
    gop_op_status_t status;
    tbx_ns_timeout_t dt;
    gop_op_generic_t *hsop;
    gop_command_op_t *hop;

    tid = tbx_atomic_thread_id;
    log_printf(15, "hc_recv_thread: New thread started! ns=%d tid=%d\n", tbx_ns_getid(ns), tid);


    firsttime = 1;  //** Flag that this is the frist time through the loop.

    //** Get the initial cmd count -- Used at the end to decide if retry
    hportal_lock(hp);
    hp->oops_recv_start++;
    start_cmds_processed = hp->cmds_processed;
    hportal_unlock(hp);

    tbx_ns_timeout_set(&dt, 1, 0);

    finished = 0;
    check_time = apr_time_now() + apr_time_make(hpc->check_connection_interval, 0);

    while (finished != 1) {
        lock_hc(hc);
        tbx_stack_move_to_bottom(hc->pending_stack);//** Get the next recv command
        hsop = (gop_op_generic_t *)tbx_stack_get_current_data(hc->pending_stack);
        unlock_hc(hc);

        if (hsop != NULL) {
            hop = &(hsop->op->cmd);

            if (tbx_atomic_inc(hop->on_top) == 0) {
                hop->start_time = apr_time_now();  //**Start the timer
                hop->end_time = hop->start_time + hop->timeout;
            }

            log_printf(5, "hc_recv_thread: before recv phase.. ns=%d gid=%d\n", tbx_ns_getid(ns), gop_id(hsop));
            status = (hop->recv_phase != NULL) ? hop->recv_phase(hsop, ns) : gop_success_status;
            hop->end_time = apr_time_now();
            log_printf(5, "hc_recv_thread: after recv phase.. ns=%d gid=%d finished=%d\n", tbx_ns_getid(ns), gop_id(hsop), status.op_status);

            lock_hc(hc);
            hc->last_used = apr_time_now();
            hc->curr_workload -= hop->workload;
            tbx_stack_move_to_bottom(hc->pending_stack);
            tbx_stack_delete_current(hc->pending_stack, 1, 0);
            hc_send_signal(hc);  //** Wake up send_thread if needed
            unlock_hc(hc);

            if ((status.op_status == OP_STATE_RETRY) && (hop->retry_count > 0)) {
                finished = 1;
                cmd_pause_time = hop->retry_wait;
                log_printf(5, "hc_recv_thread:  Dead socket so shutting down ns=%d retry in " TT " usec\n", tbx_ns_getid(ns), cmd_pause_time);
            } else if ((status.op_status == OP_STATE_TIMEOUT) && (hop->retry_count > 0)) {
                hop->retry_count--;
                log_printf(5, "hc_recv_thread: Command timed out.  Retrying.. retry_count=%d  ns=%d gid=%d\n", hop->retry_count, tbx_ns_getid(ns), gop_id(hsop));
                finished = 1;
            } else {
                log_printf(15, "hc_recv_thread:  marking op as completed status=%d retry_count=%d ns=%d gid=%d\n", status.op_status, hop->retry_count, tbx_ns_getid(ns), gop_id(hsop));
                gop_mark_completed(hsop, status);

                //**Update the number of commands processed **
                lock_hc(hc);
                hc->cmd_count++;
                unlock_hc(hc);

                hportal_lock(hp);
                hp->cmds_processed++;
                hportal_unlock(hp);
            }
        } else {
            lock_hc(hc);
            if (hc->curr_op != NULL) {  //** Start the timer if needed
                hop = &(hc->curr_op->op->cmd);
                if (tbx_atomic_get(hop->on_top) == 0) {
                    //** Need to flip the order of these locks to avoid a deadlock
                    gop_op_generic_t *curr_op = hc->curr_op;  //** Snag the top just to be safe
                    unlock_hc(hc);      //** Release it and start re-acquiring them in the proper order
                    lock_gop(curr_op);  //** Have to lock the GOP here to prevent accidental reading
                    lock_hc(hc);
                    hop->start_time = apr_time_now();  //**Start the timer
                    hop->end_time = hop->start_time + hop->timeout;
                    unlock_gop(curr_op);
                    tbx_atomic_set(hop->on_top, 1);
                }
            }

            finished = hc->shutdown_request;
            if (finished == 2) hc->shutdown_request  = 1;  //** This is used to trigger the read of the last command from the send thread
            unlock_hc(hc);
            if (finished == 0) {
                if (firsttime == 1) { //** Let the send thread know I'm up
                    lock_hc(hc);
                    hc->recv_up = 1;
                    apr_thread_cond_broadcast(hc->send_cond);
                    unlock_hc(hc);
                    firsttime = 0;
                }

                log_printf(15, "hc_recv_thread: Nothing to do so sleeping! ns=%d\n", tbx_ns_getid(ns));
                recv_wait_for_work(hc);  //** wait until we get something to do
            }
        }

        if (apr_time_now() > check_time) {  //** Time for periodic check on # threads
            log_printf(15, "hc_recv_thread: Checking if we need more connections. ns=%d\n", tbx_ns_getid(ns));
            check_hportal_connections(hp);
            check_time = apr_time_now() + apr_time_make(hpc->check_connection_interval, 0);
        }
    }

    log_printf(15, "hc_recv_thread: Exited loop! ns=%d\n", tbx_ns_getid(ns));
    log_printf(5, "hc_recv_thread: Total commands processed: %d (ns=%d, host=%s:%d)\n",
               hc->cmd_count, tbx_ns_getid(ns), hp->host, hp->port);

    //** Make sure and trigger the send if their was a problem **
    lock_hc(hc);
    hpc->fn->close_connection(ns);   //** there was an error so kill things
    hc->curr_workload = 0;
    hc->shutdown_request = 1;
    unlock_hc(hc);

    //** This wakes up everybody on the depot:( but just my end thread will exit
    hportal_lock(hc->hp);
    hportal_signal(hc->hp);
    hportal_unlock(hc->hp);
    //** this just wakes my other half up.
    lock_hc(hc);
    hc_send_signal(hc);
    unlock_hc(hc);

    //** Wait for send thread to complete **
    log_printf(5, "Waiting for send_thread to complete\n");
    apr_thread_join(&value, hc->send_thread);
    log_printf(5, "send_thread has exited\n");

    pending = 0;  //** This is used to decide if we should adjust tuning

    //** Push any existing commands to be retried back on the stack **
    if (hc->net_connect_status != 0) {  //** The connection failed
        hportal_lock(hp);
        cmds_processed = start_cmds_processed - hp->cmds_processed;
        if (cmds_processed == 0) {  //** Nothing was processed
            if (hp->n_conn == 1) {  //** I'm the last thread to try and fail to connect so fail all the tasks
                _hp_fail_tasks(hp, op_cant_connect_status);
            } else if (hp->failed_conn_attempts > hp->abort_conn_attempts) { //** Can't connect so fail
                log_printf(1, "hc_recv_thread: ns=%d failing all commands failed_conn_attempts=%d\n", tbx_ns_getid(ns), hp->failed_conn_attempts);
                _hp_fail_tasks(hp, op_cant_connect_status);
            }
        }
        hportal_unlock(hp);
    } else {
        log_printf(15, "hc_recv_thread: ns=%d stack_size=%d\n", tbx_ns_getid(ns), tbx_stack_count(hc->pending_stack));

        if (hc->curr_op != NULL) {  //** This is from the sending thread
            log_printf(15, "hc_recv_thread: ns=%d Pushing sending thread task on stack gid=%d\n", tbx_ns_getid(ns), gop_id(hc->curr_op));
            gop_hp_submit(hp, hc->curr_op, 1, 0);
            pending = 1;
        }
        if (hsop != NULL) {  //** This is my command
            log_printf(15, "hc_recv_thread: ns=%d Pushing current recving task on stack gid=%d\n", tbx_ns_getid(ns), gop_id(hsop));
            hop = &(hsop->op->cmd);
            hop->retry_count--;  //** decr in case this command is a problem
            gop_hp_submit(hp, hsop, 1, 0);
            pending = 1;
        }

        //** and everything else on the pending_stack
        while ((hsop = (gop_op_generic_t *)tbx_stack_pop(hc->pending_stack)) != NULL) {
            gop_hp_submit(hp, hsop, 1, 0);
            pending = 1;
        }
    }

    hportal_lock(hp);

    //** Now remove myself from the hportal

    hp->oops_recv_end++;
    if (hp->n_conn < 0) hp->oops_neg++;
    if (hp->n_conn > 0) hp->n_conn--;
    tbx_stack_move_to_ptr(hp->conn_list, hc->my_pos);
    tbx_stack_delete_current(hp->conn_list, 1, 0);

    log_printf(6, "hc_recv_thread: ns=%d cmd_pause_time=" TT " max_wait=%d pending=%d sleeping=%d start_stable=%d cmd_count=%d\n", tbx_ns_getid(ns), cmd_pause_time, hp->context->max_wait, pending, hp->sleeping_conn, hc->start_stable, hc->cmd_count);

    if (pending == 1) {  //** My connection was lost so update tuning params
        hp->stable_conn = hp->n_conn;
        if (hc->cmd_count < 2) hp->stable_conn--;
        if (hp->stable_conn < 0) hp->stable_conn = 0;

        if (hp->sleeping_conn > 0) cmd_pause_time = 0;  //** If already sleeping don't adjust pause time and sleep as well

        if (cmd_pause_time > 0) {
            if (cmd_pause_time > apr_time_make(hp->context->max_wait, 0)) cmd_pause_time = apr_time_make(hp->context->max_wait, 0);

            //** Check if we push out the check_hportal_connections check as well
            pause_until = apr_time_now() + cmd_pause_time;
            if ( hp->pause_until < pause_until) hp->pause_until = pause_until;
        }

        if ((hc->start_stable == 0) && (hc->cmd_count > 0)) cmd_pause_time = 0;
    }
    n = hp->n_conn;

    hp->closing_conn++;
    if (cmd_pause_time > 0) hp->sleeping_conn++;
    hportal_unlock(hp);

    log_printf(6, "hc_recv_thread: ns=%d cmd_pause_time=" TT " n_conn=%d\n", tbx_ns_getid(ns), cmd_pause_time, n);

    if (cmd_pause_time > 0) {
        if (n <= 0) {
            log_printf(6, "hc_recv_thread: ns=%d sleeping for " TT " us\n", tbx_ns_getid(ns), cmd_pause_time);
            apr_sleep(cmd_pause_time);
            log_printf(6, "hc_recv_thread: ns=%d Waking up from sleep!\n", tbx_ns_getid(ns));
        }

        hportal_lock(hp);
        hp->sleeping_conn--;
        hportal_unlock(hp);
    }

    check_hportal_connections(hp);

    log_printf(15, "Exiting routine! ns=%d host=%s\n", tbx_ns_getid(ns),hp->host);

    //** place myself on the closed que for reaping (Notice that this is done after the potentical sleep above)
    hportal_lock(hp);
    hp->closing_conn--;
    tbx_stack_push(hp->closed_que, (void *)hc);
    hportal_unlock(hp);

    apr_thread_exit(th, 0);

    return(NULL);
}


//*************************************************************
// create_host_connection - Creeats a new depot connection/thread
//*************************************************************

int create_host_connection(gop_host_portal_t *hp)
{
    gop_host_connection_t *hc;
    apr_pool_t *pool;
    apr_status_t value;
    int send_err, recv_err, err = 0;

    modify_hpc_thread_count(hp->context, 1);

    apr_pool_create(&pool, NULL);

    hc = new_host_connection(pool);
    hc->hp = hp;
    hc->last_used = apr_time_now();

    recv_err = 0;

    log_printf(3, "additional connection host=%s:%d\n", hp->host, hp->port);
    tbx_thread_create_warn(send_err, &(hc->send_thread), NULL, hc_send_thread, (void *)hc, hc->mpool);

    if (send_err == APR_SUCCESS) {
        tbx_thread_create_warn(recv_err,&(hc->recv_thread), NULL, hc_recv_thread, (void *)hc, hc->mpool);
        if (recv_err != APR_SUCCESS) {
            lock_hc(hc);
            hc->recv_up = -1;
            apr_thread_cond_broadcast(hc->send_cond);
            unlock_hc(hc);
            apr_thread_join(&value, hc->send_thread);
        }
    }

    if ((send_err != APR_SUCCESS) || (recv_err != APR_SUCCESS)) {
        hportal_lock(hp);
        hp->n_conn--;
        if (send_err != APR_SUCCESS) hp->oops_spawn_send_err++;
        if (recv_err != APR_SUCCESS) hp->oops_spawn_recv_err++;
        hportal_unlock(hp);
        modify_hpc_thread_count(hp->context, -1);

        destroy_host_connection(hc);
        err = 1;
    }

    return(err);
}

