#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>


struct kernel_task_t {
    struct kernel_task_t    *next;
    const char              *task_name;
    task_state (*callback)(const char *this_task, struct msg_t *msg, void *arg);
    void                    *arg;
    int32_t                 prio;

    xPwrMgrHandler          pm;
    task_state              is_busy;
    int32_t                 busy_without_traffic_time;
    int32_t                 busy_timeout;
    struct kernel_msg_t     *msg_queue;
    struct kernel_msg_t     *timer_msg_queue;      // !> don't allow more than one msg in queue

    task_freeze_event_callbac  freezer_callback
    bool                    task_suspended;
    bool                    task_paused;
    bool                    task_deleted;
};

bool create_task( const char *task_name,
                  task_state (*task_callback)(const char *this_task, struct msg_t *msg, void *arg),
                  void *arg,
                  int32_t prio
                  ){

    ASSERT_NULL( task_name );
    ASSERT_NULL( task_callback );
    ASSERT_TRUE( prio >= 0 );

    struct kernel_task_t * p = kernel_task_queue;

    // !> detect whether dumplicated tasks in queue
    while( p != NULL ){
    if( ! p->task_deleted ){
        if( p->task_name == task_name ){        // <! check for duplicated task name
            WARNING( "task_name[ %s ] duplicated, create failed", task_name );
            return false;
            }
        } p = p->next;
    }

    if( NULL != (p = (struct kernel_task_t *)x_malloc(sizeof(struct kernel_task_t)) ) ){
        memset( p, 0x0, sizeof(struct kernel_task_t) );
        p->task_name = task_name;
        p->callback  = task_callback;
        p->arg       = arg;
        p->prio      = prio;

        p->is_busy   = TASK_IDLE;

        // !> ALARMING when single task which in busy state last DEFAULT_BUSY_TIMEOUT ms 
        p->busy_timeout = DEFAULT_BUSY_TIMEOUT;

        // !> manage task queue due to priority: high prio(front) --> low prio(back)
        // !> BUG: the first task must be the highest priority task, or there may result in
        // !> circular linked list!!!
        struct kernel_task_t *q = kernel_task_queue, *q_prev = NULL;
        while( q != NULL ){
            if( p->prio < q->prio ){              // <! search the point to break the queue
                if( q_prev != NULL ){ q_prev->next = NULL; } break;
            } q_prev = q;  q = q->next;
        }
        MOUNT( kernel_task_queue, p );
        p->next = q;
        return true;
    }else{
        WARNING( "No memory for task_name[ %s ], create failed", task_name );
    }

    return false;
}

void show_task( void ){                
    LOG( "\r\nTask List\r\n" );
    struct kernel_task_t *p = kernel_task_queue;  int cnt = 0;
    while( p != NULL ){
        LOG( "task[%d] : %s , prio : %d\r\n", cnt++, p->task_name, p->prio );
        p = p->next;
    }
    LOG( "Task List End\r\n\r\n" );
}


static struct kernel_task_t * get_task_handler(const char *task_name, struct kernel_task_t *task_queue){
    struct kernel_task_t *t = task_queue;
    while( t != NULL ){
        if( t->task_name == task_name ){ break; }
        t = t->next;
    }

    // !> whether remain or not?
    if( t == NULL ){
        t = task_queue;
        while( t != NULL ){
            if( strcmp(t->task_name,task_name) == 0x0 ){ break; }
            t = t->next;
        }
    }
    return t;
}

bool delete_task(const char *task_name){
    struct kernel_task_t *p = get_task_handler( task_name, kernel_task_queue );
    if( p != NULL ){
        //UNMOUNT( kernel_task_queue, p );
        // TODO free msg queue .. etc.
        //x_free( p );
        p->task_deleted = true;
    }
    return true;
}

bool task_bind_freezer(const char *task_name, task_freeze_event_callback callback){
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        t->freezer_callback = callback;
        return true;
    } return false;
}

bool task_suspend( const char *task_name ){     // <! msg will be cache during suspending
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        t->task_suspended = true;
        if( t->freezer_callback != NULL ){
            t->freezer_callback( TASK_SUSPEND );
        }
        return true;
    } return false;
}

bool task_resume( const char *task_name ){      // <! msg will be delivered after resume
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        t->task_suspended = false;
        if( t->freezer_callback != NULL ){
            t->freezer_callback( TASK_RESUME );
        }
        return true;
    } return false;
}

bool task_pause( const char *task_name ){       // <! msg will not be cache during pause
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        t->task_paused = true;
        if( t->freezer_callback != NULL ){
            t->freezer_callback( TASK_PAUSE );
        }
        return true;
    } return false;
}

bool task_restart( const char *task_name ){     // <! msg will not be delivered after
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        t->task_paused = false;
        if( t->freezer_callback != NULL ){
            t->freezer_callback( TASK_RESTART );
        }
        return true;
    } return false;
}

bool task_disable_timer(const char *task_name){ // <! drop timer msg and disable timer
    struct kernel_task_t *t = get_task_handler( task_name, kernel_task_queue );
    if( t != NULL ){
        if( t->timer_msg_queue != NULL ){
            t->timer_msg_queue->timer.enable = 0;
            
            struct kernel_msg_t *m = t->timer_msg_queue;
            UNMOUNT( t->timer_msg_queue, m );
            __delete_msg( m );
        }
        return true;
    } return false;
}




