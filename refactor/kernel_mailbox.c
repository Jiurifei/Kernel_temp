#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

struct kernel_mailbox_t {
    int32_t mailbox_type : 1;         // <! mailbox and timer in 1 union, use this to determin.
    int32_t occupied     : 1;
    int32_t token        : 1;
    int32_t reserved     : 29;        // <! reserved for timer which declear above

    struct kernel_task_t *task_handler;
};

struct kernel_mailbox_group_t {
    struct kernel_mailbox_group_t *next;
    struct kernel_msg_t           *mailbox_queue;
    int32_t                       box_size;
    int32_t                       num_of_boxes;
    bool                          unread_msg;
};

/**
 *  @brief create mailbox group 
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
bool create_mailbox( int32_t mailbox_size,
                     int32_t num_of_boxes
                   ){
    /*****************************************************************************
     *                          mailbox group structure                          * 
     *                                                                           *
     *  mailbox_group1 ------> mailbox_group2 ------> mailbox_group3             *
     *         |                      |                      |                   *
     *      mailbox1               mailbox1               mailbox1               *
     *         |                      |                      |                   *
     *      mailbox2               mailbox2               mailbox2               *
     *         |                      |                      |                   *
     *      mailbox...             mailbox...             mailbox...             *
     *                                                                           *
     * Note:                                                                     *
     * 1.mailbox组按照单个mailbox的size大小由小到大加入链表中                       * 
     * 2.假设size有1 2 3，则实现上述结构需要处理以下逻辑                            *   
     *   * 3 -> 1 2(顺序)   * 2 -> 1 3(存在逆序)    * 2 -> 1 2(存在相同情况)       *
     * ***************************************************************************/
    ASSERT_TRUE( mailbox_size > 0 );
    ASSERT_TRUE( num_of_boxes > 0 );

    struct kernel_mailbox_group_t *q = kernel_mailbox_group_queue, *g = NULL;
    while( q != NULL ){
        if( mailbox_size > q->box_size ){ g = q;  q = q->next; continue; }  // <! search the insert point
        break;
    }

    // !>    situation: 2 -> 1 3 or 2 -> 1 2
    if( q != NULL ){                          // <! found (a match size) or (a insert point)
        if( mailbox_size < q->box_size ){       // <! size doesn't match
            if( g != NULL ){ g->next = NULL; }    // <! break the queue from (g) to (g->next)
        }else{ 
            g = q; goto FILL_BOXES;            // !> situation ：2 -> 1 2 3  ----> 1 2 2 3 
        }       // <! size match exactly, no need to malloc
    }else{  }                                 // <! (group_queue == NULL) or (box_size > all_exist_box)

    g = (struct kernel_mailbox_group_t *)x_malloc( sizeof(struct kernel_mailbox_group_t) );
    if( g != NULL ){
        memset( g, 0x0, sizeof(struct kernel_mailbox_group_t) );
        g->box_size = mailbox_size;
        MOUNT( kernel_mailbox_group_queue, g ); // <! MOUNT new group to queue
        g->next = q;                            // <! Join the queue together, doesn't hurt if q == NULL
    }

    FILL_BOXES:
    if( g != NULL ){
        while( num_of_boxes-- ){
            struct kernel_msg_t *p = (struct kernel_msg_t *)x_malloc( sizeof(struct kernel_msg_t) + mailbox_size );
            if( p != NULL ){
                memset( p, 0x0, sizeof(struct kernel_msg_t) + mailbox_size );

                p->mail.mailbox_type = 1;
            
                MOUNT( g->mailbox_queue, p );       // <! MOUNT to mailbox_group->mailbox_queue
                g->num_of_boxes++;                  // <! record num of malloced boxes
            }
        }
    }

    if( num_of_boxes <= 0 ){ return true; }

    WARNING( "No memory for Mailbox" );
    return false;
}

/**
 *  @brief show mailbox group and occupied mailbox info
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
void show_mailbox(void){
    LOG( "\r\nMailbox List\r\n" );

    struct kernel_mailbox_group_t *g = kernel_mailbox_group_queue;
    while( g != NULL ){
        LOG( "Mailbox[%d] x %d Bytes\r\n", g->num_of_boxes, g->box_size );
        struct kernel_msg_t *p = g->mailbox_queue;  int cnt = 0;
        while( p != NULL ){
            if( p->mail.occupied ){
                LOG( "\tbox[%d] : %s (%d)\r\n", cnt, p->msg.notification, p->msg.length );
            }
            cnt++;
            p = p->next;
        }
        g = g->next;
    }
}

