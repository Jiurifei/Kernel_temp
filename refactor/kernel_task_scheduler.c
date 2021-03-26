#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/**
 *  @brief get the minimal time before next sending uart packet 
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
int32_t try_send_tunnel_pending_packet(void){
    int32_t min_time = -1;

    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        if( mcu->is_local ){
            struct comm_tunnel_t *tunnel = mcu->tunnel;
            while( tunnel != NULL ){
                int32_t get_secure_tunnel_next_retry(struct comm_tunnel_t *tunnel);
                int32_t pending_time = get_secure_tunnel_next_retry( tunnel );
                if( pending_time >= 0 ){
                    if( min_time >= 0 ){ min_time = MIN( min_time, pending_time ); }
                    else{ min_time = pending_time; }
                }
                tunnel = tunnel->next;
            } break;
        }
        mcu = mcu->next;
    }

    return min_time;
}

/**
 *  @brief calculate sleep time between now and next work state
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
uint32_t kernel_idle_time(void){
    struct kernel_task_t *t = kernel_task_queue;
    uint32_t min = 0xFFFFFFFF;

    struct kernel_mailbox_group_t *g = kernel_mailbox_group_queue;
    while( g != NULL ){
        if( g->unread_msg ){ return 0; }              // <! Mailbox has unread msg
        g = g->next;
    }

    // !> calculate the minimal delay time of local task 
    while( t != NULL ){
        if( t->is_busy != TASK_IDLE ){ return 0; }    // <! Task isn't IDLE

        if( NULL != (t->timer_msg_queue) ){
            struct kernel_msg_timer_t *timer = &(t->timer_msg_queue->timer);

            if( timer->enable ){
                if( (uint32_t)timer->delay < min ){ min = timer->delay; }
            }
        } t = t->next;
    }

    // !> calculate the minimal pending time of sending uart packet
    int32_t try_send_tunnel_pending_packet(void);
    int32_t pending_time = try_send_tunnel_pending_packet();
    if( pending_time >= 0 ){
        min = MIN( (uint32_t)pending_time, min );
    }

    // !> calculate the minimal time interval before next Synchronizing core 
    // !> Synchronize core: triggered when local task has changed 
    int32_t mmap_retry_time = get_kernel_mmap_unsync_timeout();
    if( mmap_retry_time >= 0 ){
        min = MIN( (uint32_t)mmap_retry_time, min );
    }

    return min;
}

void kernel_task_sheduler(void){
    struct kernel_task_t *t = kernel_task_queue;
    struct kernel_msg_t *m = NULL;

    static int32_t t1 = 0;
    int32_t t2 = 0, delta_ms = 0;
    t2 = kernel_get_tick_callback();     delta_ms = t2 - t1;     t1 = t2;

    extern void pwr_mgr_timer_update(int32_t delta_ms);
    pwr_mgr_timer_update( delta_ms );

    /**********************************************************************
     |                                                                     |
    |                  Convert Mailbox to Kernel Message                  |
    |                                                                     |
    **********************************************************************/

    #ifdef PTHREAD_H
    pthread_mutex_lock( &isr_mutex );
    #endif
    {
    // !> copy all of the mailbox messages to msg_queue of aimed task 
    struct kernel_mailbox_group_t *g = kernel_mailbox_group_queue;
    while( g != NULL ){
        if( g->unread_msg ){                    // <! only enter when mailbox has unread mail
            g->unread_msg = false;                // <! we are going to read all the boxes
                                                // <! in case new message arrived while fetch mail, we clear here
                                                // <! so the unread flag won't be accidentally clear in the process
            m = g->mailbox_queue;
            while( m != NULL ){
                if( m->mail.occupied ){             // <! get the occupied mailbox
                    if( m->mail.task_handler != NULL ){
                        struct kernel_msg_t *nmsg = new_msg( m->msg.notification, m->msg.data, m->msg.length );
                        if( nmsg != NULL ){
                            t = m->mail.task_handler;
                            nmsg->time_stamp  = m->time_stamp;
                
                            MOUNT( t->msg_queue, nmsg );    // <! duplicate the msg from mailbox
                            memset( &(m->msg), 0x0, sizeof(struct msg_t) );
                            m->mail.task_handler = NULL;
                        }
                        m->mail.token     = 0;          // <! release token
                        m->mail.occupied  = 0;
                    }else{
                        g->unread_msg = true;           // <! Message is unread
                    }
                }
                m = m->next;
            }
        }
        g = g->next;
    }
    }
    #ifdef PTHREAD_H
    pthread_mutex_unlock( &isr_mutex );
    #endif

    /**********************************************************************
     |                                                                     |
    |             Check Timer Message and Duplicate if needed             |
    |                                                                     |
    **********************************************************************/
   // !> timer_msg timeout, put it into msg_queue 
    if( delta_ms > 0 ){   
        t = kernel_task_queue;                      // <! only run when delta > 0
        while( t != NULL ){                                             // <! run through all task
            if( NULL != (m = t->timer_msg_queue) ){
                if( m->timer.enable ){
                    if( m->timer.delay > delta_ms ){ 
                        m->timer.delay -= delta_ms;                             // <! sub delta_ms
                    }else{ 
                        m->timer.delay  = 0;                                    // <! delay counter reach 0
                        if( ! t->task_suspended ){                              // <! will not deliver when task is suspended
                            if( (m->timer.preodic > 0) && (m->timer.cnt != 0) ){  // <! (cnt < 0): infinite loop
                                if( m->timer.cnt > 0 ){  m->timer.cnt--; }          // <! sub repeat counter
                                m->timer.delay = m->timer.preodic;                  // <! reload preodic timer
                                m = kernel_duplicate_msg( m );                      // <! duplicate a new message, m will be different after!!
                            }else{
                                m->timer.enable = 0;                                // <! disable timer
                                t->timer_msg_queue = NULL;                          // <! drop timer from queue, only one message in queue
                            }
                            MOUNT( t->msg_queue, m );                             // <! MOUNT to msg queue
                        }
            
                        if( ! t->task_paused ){
                            t->is_busy |= TASK_MSG_PENDING; //t->is_busy = TASK_BUSY;
                        }
                    }
                }
            }
            t = t->next;
        }
    }

    t = kernel_task_queue;
    while( t != NULL ){
        // <! 3. check power state
        extern bool pwr_mgr_activate(xPwrMgrHandler pm);
        extern bool pwr_mgr_diactivate(xPwrMgrHandler pm);
        extern pwr_state_t pwr_mgr_check(xPwrMgrHandler pm);
        extern bool pwr_mgr_check_power_failure(xPwrMgrHandler pm);

    /**********************************************************************
    |                                                                     |
    |                      Check Task Power State                         |
    |                                                                     |
    |          Diactivate or Activate Power of Task accordingly           |
    |                                                                     |
    **********************************************************************/

        if( pwr_mgr_check(t->pm) == POWER_DIACTIVATING ){           // <! go on diactivate if POWER_DIACTIVATING
            pwr_mgr_diactivate( t->pm );                            // <! can't process any msg right now.
        }else{
            if( t->task_paused ){                                   // <! Drop all msg when task is paused.
                while( NULL != (m = t->msg_queue) ){
                    UNMOUNT( t->msg_queue, m );  __delete_msg( m );
                }
                t->is_busy = TASK_IDLE;                               // <! TODO
    //          while( NULL != (m = t->timer_msg_queue) ){          // <! Do not delete the timer msg
    //            UNMOUNT( t->timer_msg_queue, m );  __delete_msg( m );
    //          }
            }

            if( (NULL != (m = t->msg_queue)) && (!t->task_suspended) ){
                if( t->pm != NULL ){
                    if( pwr_mgr_check_power_failure(t->pm) ){           // <! check power failure, default 3 times
                        WARNING( "task[ %s ] Power Failure, Droping Msg [%s]", t->task_name, m->msg.notification );
                        t->is_busy = TASK_IDLE;                           // <! assume task is IDLE because no msg will be deliver
                        goto DROP_MSG;
                    }
                    if( ! pwr_mgr_activate(t->pm) ){                    // <! Try to Active Power for Task
                        if( pwr_mgr_check(t->pm) == POWER_GIVE_UP_ACTIVATE ){
                            WARNING( "task[%s] power give up activate", t->task_name );
                            while( NULL != (m = t->msg_queue) ){            // <! Clear All pending msg when power give up
                                 WARNING( "Droping Msg [%s] (%d)", m->msg.notification, m->msg.length );
                                 UNMOUNT( t->msg_queue, m );  __delete_msg( m );
                            }
                            LOG("t->msg_queue %p\n", t->msg_queue);
                            t->is_busy = TASK_IDLE;                         // <! Must set IDLE, incase BUSY is set when post_msg
                        }
                        goto NEXT_TASK;                                   // <! can't deliver msg until power activated
                    }
                }
            
            
            /**********************************************************************
             |                                                                     |
            |       Sort Message by TimeStamp, In case Communication error        |
            |                                                                     |
            **********************************************************************/
            
                {
                struct kernel_msg_t *p = t->msg_queue;  int32_t max = 0;
                while( p != NULL ){
                    int32_t time = tock_us( p->time_stamp );
                    if( time > max ){ max = time;  m = p; }
                    p = p->next;
                }
                }
            
            /**********************************************************************
             |                                                                     |
            |        Power Already Activated, Deliver Message to Task Now         |
            |                                                                     |
            **********************************************************************/

                if( t->callback != NULL ){
    //            if( strcmp(t->task_name, "chips_uart_task") == 0 ){
    //              printf( "uart[%d]: %d\n", m->msg.length, m->time_stamp );
    //            }
                    int32_t t1 = kernel_get_tick_callback();
                    task_state ret = t->callback( t->task_name, &(m->msg), t->arg );
                    t->is_busy &=~ TASK_MSG_PENDING;
                    switch( ret ){
                        case TASK_IGNORE : break;
                        default          : t->is_busy = ret;
                    }
                    int32_t t2 = kernel_get_tick_callback();
            
                    // !> feed watchdog
                    watchdog_feed();

                    // !> sign task which runs more than 200ms
                    if( (t2 - t1) > 200 ){
                        WARNING( "task[ %s ] Process [%s] took %d ms", t->task_name, m->msg.notification, (t2 - t1) );
                    }
                }else{ t->is_busy = TASK_IDLE; }                      // <! fail-safe normally won't reach here

            DROP_MSG:
                UNMOUNT( t->msg_queue, m );
                __delete_msg( m );
            
            //if( t->msg_queue != NULL ){ t->is_busy = TASK_BUSY; } // <! Keep task busy if msg_queue available
                if( t->msg_queue != NULL ){ t->is_busy |= TASK_MSG_PENDING; } // <! Keep task busy if msg_queue available

            /**********************************************************************
             |                                                                     |
            |    Power will be Diactivate if Task require to Sleep immediately    |
            |                                                                     |
            |   Also  Collect busy without traffic time to Detect Abnormal Task   |
            |                                                                     |
            **********************************************************************/

                switch( t->is_busy ){
                    case TASK_READY_TO_SLEEP :                          // <! Task require to sleep immediately
                        pwr_mgr_diactivate( t->pm );                // <! Diactivate power immediately
                        t->is_busy = TASK_IDLE;                     // <! Reset Task state to IDLE for whole system to sleep
                    break;
                    
                    case TASK_BUSY | TASK_MSG_PENDING :
                    case TASK_IDLE | TASK_MSG_PENDING :
                    case TASK_BUSY :                                    // <! Task busy with new msg, clear the busy time
                    case TASK_IDLE :
                        t->busy_without_traffic_time = 0;           // <! reset collect timer
                        t->busy_timeout = DEFAULT_BUSY_TIMEOUT;     // <! reload timeout setting
                    break;
            
                    default : break;
                }
            }else{
                                                                // <! No New Msg
                if( (t->is_busy & TASK_MSG_PENDING) || (t->is_busy == TASK_BUSY) ){                        // <! Task Busy without Traffic
                    t->busy_without_traffic_time += delta_ms;           // <! collect continuous busy time (unit:ms)
            
                    if( t->busy_without_traffic_time > t->busy_timeout ){// <! continuous busy for 3 minute
                        WARNING( "task[ %s ] busy with No Traffic for over %d minutes", 
                        t->task_name, t->busy_without_traffic_time / (60 * 1000) );
                        t->busy_timeout += 60 * 1000;                     // <! Warning will be print in next minute
                    }
                }
            }
        
        /**********************************************************************
        |                                                                     |
        |                         Task delete procedure                       |
        |                                                                     |
        **********************************************************************/

            if( t->task_deleted ){                                  // <! Task Deleting
                struct kernel_task_t *next = t->next;
                UNMOUNT( kernel_task_queue, t );                       // <! clear from task queue
            // <! TODO   delete msg
                {
                struct kernel_msg_t *m = t->msg_queue;               // <! clear msg_queue
                while( NULL != (m = t->msg_queue) ){
                    UNMOUNT( t->msg_queue, m );
                    __delete_msg( m );
                }
                }
                {
                struct kernel_msg_t *m = t->timer_msg_queue;         // <! clear timer_msg_queue
                while( NULL != (m = t->timer_msg_queue) ){
                    UNMOUNT( t->timer_msg_queue, m );
                    __delete_msg( m );
                }
                }
                x_free( t );                                           // <! free resource
                t = next;
                continue;
            }
        }

        NEXT_TASK:
        t = t->next;
    }

    int32_t try_send_tunnel_pending_packet(void);
    try_send_tunnel_pending_packet();

    kernel_mmap_update_to( NULL, true );
    kernel_mmap_check_unsync_core( 0 );
}


