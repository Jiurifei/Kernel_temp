#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

struct kernel_msg_timer_t {
    int32_t reserved : 3;             // <! reserved for mailbox which declear below
    int32_t enable   : 1;             // <! timer enable
    int32_t cnt      : 28;            // <! repeat counter

    int32_t delay;                    // <! delay : unit( ms )
    int32_t preodic;                  // <! preodic period : unit( ms )
};

#pragma anon_unions        // !> 匿名结构体/联合体
struct kernel_msg_t {
    struct kernel_msg_t       *next;

    // !> msg produced: 1.msg_from_app  2.msg_from_isr
    union {
        struct kernel_msg_timer_t timer;       
        struct kernel_mailbox_t   mail;
    };
    int32_t time_stamp;

    struct msg_t              msg;
};

char * __strdup__(const char *src){
    if( src == NULL ){ return NULL; }

    char *dst = x_malloc( strlen(src) + 1 );
    if( dst != NULL ){
        strcpy( dst, src );
    }else{
        WARNING( "no mem" );
    }
    return dst;
}

/**
 *  @brief duplicate msg
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static struct kernel_msg_t * kernel_duplicate_msg(const struct kernel_msg_t *src){
    struct kernel_msg_t *p = (struct kernel_msg_t *)x_malloc( sizeof(struct kernel_msg_t) + src->msg.length + 1 );
    if( p != NULL ){
        memset( p, 0x0, sizeof(struct kernel_msg_t) + src->msg.length + 1 );

        p->msg.notification = __strdup__( src->msg.notification );
        p->time_stamp       = src->time_stamp;
        p->msg.length       = src->msg.length;
        memcpy( p->msg.data, src->msg.data, src->msg.length );

        if( p->msg.notification == NULL ){
            WARNING( "No Memory for dup msg[%s]\r\n", src->msg.notification );
            x_free( p );  p = NULL;
        }
    }else{ WARNING( "No Memory for dup msg[%s]\r\n", src->msg.notification ); }
    return p;
}

/**
 *  @brief delete msg : clear flag / release memory
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static void __delete_msg(struct kernel_msg_t *p){
    ASSERT_NULL( p );
    if( p == NULL ){ return; }

    if( ! p->mail.mailbox_type ){
        // !>  malloc from new_msg
        if( p->msg.notification != NULL ){
            x_free( (void *)p->msg.notification );
        }
        if( p->msg.src_task != NULL ){
            x_free( (void *)p->msg.src_task );
        }
        x_free( p );
    }else{
        p->mail.task_handler = NULL;
        p->mail.token = 0;
        p->mail.occupied = 0;
    }
}

/**
 *  @brief new message when in application
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMsgHandler __new_msg(const char *notification, char *data, int32_t length){
    ASSERT_NULL( notification );
    ASSERT_TRUE( length >= 0 );

    #ifdef GET_IPSR
    if( get_ipsr() > 0 ){  ERROR( "new_msg[%s] inside Interrupt is not allowed", notification ); }
    #endif

    struct kernel_msg_t * p = 
            (struct kernel_msg_t *)x_malloc( sizeof(struct kernel_msg_t) + length + 1 );  // <! plus '1' for end of strings
    if( NULL != p ) {
        memset( p, 0x0, sizeof(struct kernel_msg_t) + length + 1 );
        p->msg.length        = length;
        p->msg.notification  = __strdup__( notification );
        p->time_stamp        = tick_us();
        p->next = NULL;

        if( (length > 0) && (NULL != data) ) {
            memcpy( p->msg.data, data, length );
        }

        if( p->msg.notification == NULL ){
            WARNING( "No Memory for msg[%s]\r\n", notification );
            x_free( p );  p = NULL;
        }
    }else{ WARNING( "No Memory for msg[%s]\r\n", notification ); }
    return (xMsgHandler)p;
}

xMsgHandler __new_str(const char *notification, char *str){
    ASSERT_NULL( str );
    if( str == NULL ){ return NULL; }
    return __new_msg( notification, str, strlen(str) );
}

xMsgHandler __new_notification(const char *notification){
    return __new_msg( notification, NULL, 0 );
}

/**
 *  @brief new message when in interrupt
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMsgHandler __new_msg_from_isr(const char *notification, char *data, int32_t length){
    ASSERT_NULL( notification );
    ASSERT_TRUE( length >= 0 );

    #ifdef GET_IPSR
    if( get_ipsr() == 0 ){  WARNING( "new_msg[%s] outside of Interrupt is not recommended", notification ); }
    #endif

    #ifdef PTHREAD_H
    pthread_mutex_lock( &isr_mutex );
    #endif

    struct kernel_mailbox_group_t *g = kernel_mailbox_group_queue;
    while( g != NULL ){
        if( g->box_size > length + 1 ){
            struct kernel_msg_t *p = g->mailbox_queue;
            while( p != NULL ){
                if( ! p->mail.occupied ){
                    if(  p->mail.token   ){ p = p->next; continue; }
                    p->mail.token = 1;

                    if( p->mail.occupied ){ p = p->next; continue; }
                    p->mail.occupied    = 1;
                    p->msg.src_task     = NULL;
                    p->msg.notification = notification;
                    p->msg.length       = length;
                    memcpy( p->msg.data, data, length );
                    p->msg.data[length] = 0;
                    p->time_stamp       = tick_us();

                    g->unread_msg       = true;
                    
                    #ifdef PTHREAD_H
                    pthread_mutex_unlock( &isr_mutex );
                    #endif

                    return p;
                }
                p = p->next;
            }
        }
        g = g->next;
    }

    #ifdef PTHREAD_H
    pthread_mutex_unlock( &isr_mutex );
    #endif

    WARNING( "No Mailbox for msg[%s]\r\n", notification );
    show_mailbox();
    return NULL;
}

xMsgHandler __new_str_from_isr(const char *notification, char *str){
    ASSERT_NULL( str );
    if( str == NULL ){ return NULL; }
    return __new_msg_from_isr( notification, str, strlen(str) );
}

xMsgHandler __new_notification_from_isr(const char *notification){
    return __new_msg_from_isr( notification, NULL, 0 );
}

/**
 *  @brief set msg delay and periodic parameters
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMsgHandler msg_set_repeat_n_timer(xMsgHandler msg, int32_t delay, int32_t preodic, int32_t cnt){
    ASSERT_NULL( msg );

    if( msg == NULL ){ return NULL; }

    struct kernel_msg_t *p = msg; 
    if( p->mail.mailbox_type ){ return NULL; }    // <! msg from mailbox is not allow to set timer

    if( cnt > 0 ){ cnt--; }

    p->timer.enable  = 1;                         // <! enable timer function
    p->timer.delay   = delay;                     // <! set parameters
    p->timer.preodic = preodic;
    p->timer.cnt     = cnt;

    return (xMsgHandler)msg;                      // <! return handler for multi-level
}

xMsgHandler msg_set_repeat_timer(xMsgHandler msg, int32_t delay, int32_t preodic){
    return msg_set_repeat_n_timer(msg, delay, preodic, -1);
}

xMsgHandler msg_set_delay_timer(xMsgHandler msg, int32_t delay){
    return msg_set_repeat_n_timer(msg, delay, -1, -1);
}

  /**********************************************************************
  |                                                                     |
  |                        message transmission                         |
  |                                                                     |
  **********************************************************************/

static bool try_post_msg_outside(const char *target_task, struct kernel_msg_t *msg, const char *src_task);

/**
 *  @brief post msg from source task to target task
 * 
 *  @param [in] 
 *  @param [out]
 *  @return 
 **/
bool __post_msg_from(const char *target_task, xMsgHandler msg, const char *src_task){
    ASSERT_NULL( target_task );
    ASSERT_NULL( msg );

    if( msg == NULL ){ return false; }
    if( target_task == NULL ){ goto ERR; }

    struct kernel_task_t *t = get_task_handler( target_task, kernel_task_queue );
    struct kernel_msg_t *p = (struct kernel_msg_t *)msg;

    // !> t == NULL means task belongs to outside cores
    if( t == NULL ){
        if( try_post_msg_outside(target_task, p, src_task) ){
            __delete_msg( msg );
            return true;
        }
    }

    //ASSERT_NULL( t );
    if( t == NULL ){ goto ERR; }

    if( t->task_paused ){ goto ERR; }           // <! task has been paused.

    if( p->mail.mailbox_type ){                 // <! msg from mailbox
        #ifdef PTHREAD_H
        pthread_mutex_lock( &isr_mutex );
        #endif
        p->mail.task_handler = t;
        if( src_task != NULL ){
            ERROR( "Msg[%s] From ISR Should not have src_task[%s]", p->msg.notification, src_task );
        }
        t->is_busy |= TASK_MSG_PENDING; //t->is_busy = TASK_BUSY;
        #ifdef PTHREAD_H
        pthread_mutex_unlock( &isr_mutex );
        #endif
    }else{
        p->msg.src_task = __strdup__( src_task );

        if( p->timer.enable ){                    // <! timer enable
            if( t->timer_msg_queue != NULL ){
                WARNING( "Droping older timer : [%s]", t->timer_msg_queue->msg.notification );
                __delete_msg( t->timer_msg_queue );   // <! only 1 timer msg is allow
            }
            t->timer_msg_queue = p;                 // <! switch to new msg
        }else{
            MOUNT( t->msg_queue, p );               // <! normal message, mount to msg queue
            t->is_busy |= TASK_MSG_PENDING; //t->is_busy = TASK_BUSY;
        }
    }

    return true;

    ERR:
    if( ! p->mail.mailbox_type ){
        WARNING( "Error occur when post to [%s], msg Drop!", target_task );
    }
    __delete_msg( msg );                        // <! drop msg if error occur
    return false;
}

bool __post_msg(const char *target_task, xMsgHandler msg){
  return __post_msg_from( target_task, msg, NULL );
}



