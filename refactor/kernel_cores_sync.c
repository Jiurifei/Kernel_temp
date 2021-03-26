#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/*************************************************************************

           -------------------------------------------------
          |                                                 |
          |     Kernel Communication Between Other MCUs     | 
          |                                                 |
           -------------------------------------------------
           
*************************************************************************/

struct kernel_external_task_t {
    struct kernel_external_task_t *next;
    bool                          cached;                 // <! Mark as recoverd task, can not use for sync
    #if defined (DISABLE_NON_ZERO_ARRAY)
    char                          task_name[1];           // <! [1] Special for ARMCC which Not support ZeroArray
    #else
    char                          task_name[0];           // <! Task name , alloc in same ram space
    #endif
};

struct MCUs_t {
    struct MCUs_t                 *next;
    struct kernel_external_task_t *task_queue;            // <! Task queue of MCU
    struct comm_tunnel_t          *tunnel;                // <! Send out Tunnel
    int                           jump;                   // <! Jump point of the MCU
    bool                          is_local;               // <! Local MCU Mark
    bool                          support_json_extra;     // <! support Hex data outside of JSON
    bool                          mmap_req_sent;          // <! mmap req sent marker, only req once
    bool                          task_modified;          // <! Use for backup marker
    #if defined (DISABLE_NON_ZERO_ARRAY)
    char                          core[1];                // <! [1] Special for ARMCC which Not support ZeroArray
    #else
    char                          core[0];                // <! Task name , alloc in same ram space
    #endif
};

static struct MCUs_t *kernel_mcu_queue = NULL;
static bool all_support_json_extra = false;
const char *local_core_name = NULL;


static struct MCUs_t * is_mcu_exist(const char *core_name){
    ASSERT_NULL( core_name );
    struct MCUs_t *p = kernel_mcu_queue;
    while( p != NULL ){
        if( strcmp(p->core, core_name) == 0x0 ){ break; }
        p = p->next;
    }
    return p;
}

static const char * get_my_core_name(void){
    struct MCUs_t *p = kernel_mcu_queue;
    while( p != NULL ){
        if( p->is_local ){ return p->core; }
        p = p->next;
    }
    return NULL;
}

/*************************************************************************

           -------------------------------------------------
          |                                                 |
          |     Kernel Memory Mapping Between Other MCUs    | 
          |                                                 |
           -------------------------------------------------
           
*************************************************************************/

#pragma anon_unions
struct kernel_mmap_t {
    struct kernel_mmap_t  *next;
    union {
        char                *from_core;
        char                *to_core;
    };
    char                  *mem_name;
    void                  *prev_sync_mem;
    void                  *mem;
    int32_t               mem_size;
    bool                  sync_already;

    mmap_update_notify    update_callback;
    void                  *arg;
};

static struct kernel_mmap_t *kernel_mmap_from_queue = NULL, *kernel_mmap_to_queue = NULL;

struct kernel_mmap_t *create_kernel_mmap(const char mem_name[], void *mem, int32_t mem_size){
    ASSERT_NULL( mem );
    ASSERT_NULL( mem_name );

    const char *__keyword[] = { "mmap_array", "mmap" };
    for( unsigned int i=0; i<sizeof(__keyword)/sizeof(__keyword[0]); i++ ){
        if( strcmp(__keyword[i], mem_name) == 0x0 ){
            ERROR( "mmap name conflic [%s]", __keyword[i] );
        }
    }

    struct kernel_mmap_t *p = (struct kernel_mmap_t *)x_malloc( sizeof(struct kernel_mmap_t) );
    if( p != NULL ){
        memset( p, 0x0, sizeof(struct kernel_mmap_t) );
        p->mem_name  = __strdup__( mem_name );
        p->mem      = mem;
        p->mem_size = mem_size;
    }
    return p;
}

/**
 *  @brief create local receiving memory mapping space 
 *         aim: sync "mem_name" memory from "core_name" core
 *         mem_pointer: "mem"   mem_size: "mem_size" 
 *         REMOTE -> LOCAL
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMmapHandler kernel_mmap_from(const char core_name[], const char mem_name[], void *mem, int32_t mem_size){
    ASSERT_NULL( core_name );
    ASSERT_NULL( mem_name );
    ASSERT_NULL( mem );
    ASSERT_TRUE( mem_size > 0 );

    struct kernel_mmap_t *p = create_kernel_mmap( mem_name, mem, mem_size );
    if( p != NULL ){
        p->from_core = __strdup__( core_name );
        MOUNT( kernel_mmap_from_queue, p );
    }
    return p;
}

/**
 *  @brief create local sending memory mapping space
 *         aim: sync "mem_name" memory to "core_name" core
 *         mem_pointer: "mem"   mem_size: "mem_size" 
 *         LOCAL -> REMOTE
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMmapHandler kernel_mmap_to(const char core_name[], const char mem_name[], void *mem, int32_t mem_size){
    ASSERT_NULL( core_name );
    ASSERT_NULL( mem_name );
    ASSERT_NULL( mem );
    ASSERT_TRUE( mem_size > 0 );

    struct kernel_mmap_t *p = create_kernel_mmap( mem_name, mem, mem_size );
    if( p != NULL ){
        p->prev_sync_mem = x_malloc( mem_size );
        if( p->prev_sync_mem == NULL ){ x_free(p); return false; }

        memset( p->prev_sync_mem, 0x0, mem_size );
        memcpy( p->prev_sync_mem, mem, mem_size );
        p->to_core = __strdup__( core_name );
        MOUNT( kernel_mmap_to_queue, p );
    }
    return p;
}

/**
 *  @brief register callback handler which called when receiving remote core sync request
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
xMmapHandler kernel_mmap_set_update_callback(xMmapHandler handler, mmap_update_notify callback, void *arg){
    struct kernel_mmap_t *p = kernel_mmap_from_queue;
    while( p != NULL ){
        if( p == handler ){ break; }
        p = p->next;
    }

    if( p != NULL ){
        p->update_callback = callback;
        p->arg             = arg;
    }
    return p;
}

static int32_t mmap_check_unsync_timeout = -1;
static int32_t get_kernel_mmap_unsync_timeout(void){
    return mmap_check_unsync_timeout;
}

/**
 *  @brief send out msg to destination core by using tunnel
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static bool kernel_router_raw(const char *dst_core, void *msg, int32_t len, struct comm_tunnel_t *avoid_tunnel){
    if( dst_core == NULL ){ return false; }
    if( msg == NULL )     { return false; }           // <! the msg will be freed by internal
    if( len == 0x00 )     { return true;  }

    #ifdef PTHREAD_H
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock( &mutex );
    #endif

    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        if( strcmp(mcu->core, dst_core) == 0x0 ){
        
            if( mcu->tunnel == avoid_tunnel ){ break; }                           // <! avoid same channel
        
            layer_proc_func_list *proc = mcu->tunnel->send_proc;
            int32_t sent_length = proc[0].func( &proc[1], mcu->tunnel, (uint8_t *)msg, len );
            #ifdef PTHREAD_H
            pthread_mutex_unlock( &mutex );
            #endif
            return true;
        }
        mcu = mcu->next;
    }
    #ifdef PTHREAD_H
    pthread_mutex_unlock( &mutex );
    #endif
    return false;
}

/**
 *  @brief Reorganize Json structure to string and padding data if exists
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static bool kernel_router_json(const char *dst_core, cJSON *js, void *extra_data, uint16_t length, struct comm_tunnel_t *avoid_tunnel){
    if( dst_core == NULL ){ return false; }
    if( js == NULL )      { return false; }

    char *jsString = cJSON_PrintUnformatted( js );
    if( jsString != NULL ){
        LOG( "%s\r\n", jsString );

        // !> padding data to Json
        if( (extra_data != NULL) && (length > 0) ){
            int with_extra_length = strlen(jsString) + 1 + length
                            + sizeof(struct json_extra_data_t)
                            - sizeof( ((struct json_extra_data_t *)0)->data );
            char *s = (char *)x_malloc( with_extra_length );
            if( s != NULL ){
                memcpy( &s[0], jsString, strlen(jsString) + 1 );
                struct json_extra_data_t *p = (struct json_extra_data_t *)&s[ strlen(jsString) + 1 ];
                p->data_type = cJSON_HexString;
                p->length = length;
                memcpy( p->data, extra_data, length );
                x_free( jsString );
                return kernel_router_raw( dst_core, s, with_extra_length, avoid_tunnel );
            }else{
                x_free( jsString );
                return false;
            }
        }

        return kernel_router_raw( dst_core, jsString, strlen(jsString) + 1, avoid_tunnel );
    }
    return false;
}

/**
 *  @brief pack core sync request in Json structure
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static bool kernel_mmap_request(const char *src_core, const char *dst_core, 
                                struct comm_tunnel_t *avoid_tunnel){
    if( src_core == NULL ){ return false; }
    if( dst_core == NULL ){ return false; }

    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        if( strcmp(mcu->core, src_core) == 0x0 ){               // <! Specially match the "src_core"
            
            if( mcu->tunnel == avoid_tunnel ){ break; }           // <! avoid same channel
            
            if( mcu->tunnel->passive_tunnel ){                                    // <! Passive Tunnel
                if( ! mcu->tunnel->tunnel_enabled ){                                // <! Tunnel Disabled
                    return false;                                                     // <! We don't send anything outside
                }
            }
        
        /*************************************************************************
         *                                                                        *
         *          -------------------------------------------------             *
         *         |                                                 |            *
         *         |        Process mmap request  From JSON          |            *
         *         |                                                 |            *
         *          -------------------------------------------------             *
         * mmap_sync_req                                                          *
         * {                                                                      *
         *     "src_core": "nRF52840",                                            *
         *     "dst_core": "PSoc6_M0",                                            *
         * }                                                                      *
         *                                                                        *
         *************************************************************************/
        
            cJSON *js = cJSON_CreateObject();
            if( js != NULL ){
                cJSON *mmap_js = cJSON_CreateObject();
                if( mmap_js != NULL ){
                    cJSON_AddItemToObject( js, "mmap_sync_req", mmap_js );
                    
                    cJSON_AddStringToObject( mmap_js, "src_core", src_core );      // <! source from
                    cJSON_AddStringToObject( mmap_js, "dst_core", dst_core );      // <! source from
                    
                    bool ret = kernel_router_json( mcu->core, js, NULL, 0, avoid_tunnel );
                    cJSON_Delete( js );
                    return ret;
                }

                cJSON_Delete(js); 
                return false;
            }
        }
        mcu = mcu->next;
    }
    WARNING( "src_core [%s] not found", src_core );
    return false;
}

/**
 *  @brief check whether send sync core request!
 *         condition: core or tasks of core has changed
 *         first call: timeout_set >= 0
 *         later call: timeout_set = 0, in kernel_task_scheduler
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static bool kernel_mmap_check_unsync_core(int32_t timeout_set){
    static int32_t timeout = -1;
    static int32_t t0 = 0;

    // !> first call timeout_set may > 0
    if( timeout_set > 0 ){  
        mmap_check_unsync_timeout = timeout_set;                      // <! timeout setting
        t0 = tick(); 
        LOG("timeout set %d\n", timeout_set); 
    }

    if( (mmap_check_unsync_timeout != -1) && (tock(t0) > mmap_check_unsync_timeout) ){// <! check unsync after tiemout
        LOG( "time to try sync mmap\n" );
        mmap_check_unsync_timeout = -1;

        struct MCUs_t *mcu = kernel_mcu_queue;
        while( mcu != NULL ){ mcu->mmap_req_sent = false; mcu = mcu->next; }

        struct kernel_mmap_t *p = kernel_mmap_from_queue;
        while( p != NULL ){
            if( ! p->sync_already ){                                      // <! Ignore the core that already sync
                struct MCUs_t *mcu = is_mcu_exist( p->from_core );
                if( mcu != NULL ){
                    if( ! mcu->mmap_req_sent ){
                        // <! send mmap request
                        kernel_mmap_request( p->from_core, get_my_core_name(), NULL );
                        mcu->mmap_req_sent = true;
                        // <! Could cause multi-req to one core per call  TO DO
                    }
                }
            }
            p = p->next;
        }
    }
    return true;
}

/**
 *  @brief sync shared memory message of peer core to local when it has changed
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static void kernel_mmap_update_from(const char core_name[], const char mem_name[], void *mem, int32_t mem_size){
    ASSERT_NULL( core_name );
    ASSERT_NULL( mem_name );
    ASSERT_NULL( mem );
    ASSERT_TRUE( mem_size > 0 );

    struct kernel_mmap_t *p = kernel_mmap_from_queue;
    while( p != NULL ){
        if( strcmp(p->from_core, core_name) == 0x0 ){
            if( is_mcu_exist(p->from_core) != NULL ){
                if( strcmp(p->mem_name, mem_name) == 0x0 ){
                    if( p->mem_size == mem_size ){
                        p->sync_already = true;
                        //LOG( "mem[%s] updated %d Bytes from [%s]\n", p->mem_name, mem_size, p->from_core );
                        if( memcmp(p->mem, mem, p->mem_size) != 0x0 ){              // <! We don't update if excetly the same
                            memcpy( p->mem, mem, p->mem_size );
                            if( p->update_callback != NULL ){
                                // !> the callbck of shared memory
                                p->update_callback( p->arg, p->mem, p->mem_size );
                            }
                        }
                        return;
                    }
                }
            }
        }
        p = p->next;
    }
}

/**
 *  @brief pack shared memory message of local core and send it out
 * 
 *  @param [in]
 *  @param [out]
 *  @return 
 **/
static bool kernel_mmap_outside(const char *src_core, const char *dst_core, const char *mem_name,
                                void *mem_data, int32_t mem_size,
                                struct comm_tunnel_t *avoid_tunnel ){
    if( src_core == NULL ){ return false; }
    if( dst_core == NULL ){ return false; }
    if( mem_name == NULL ){ return false; }
    if( mem_data == NULL ){ return false; }
    if( mem_size == 0x00 ){ return false; }

    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        if( strcmp(mcu->core, dst_core) == 0x0 ){
            if( mcu->tunnel == avoid_tunnel ){ break; }                           // <! avoid same channel
        
            if( mcu->tunnel->passive_tunnel ){                                    // <! Passive Tunnel
                if( ! mcu->tunnel->tunnel_enabled ){                                // <! Tunnel Disabled
                    return false;                                                     // <! We don't send anything outside
                }
            }
      
      /*************************************************************************
      *                                                                        *
      *          -------------------------------------------------             *
      *         |                                                 |            *
      *         |    Try to Sync kernel memory mapping  To JSON   |            *
      *         |                                                 |            *
      *          -------------------------------------------------             *
      *                                                                        *
      * {                                                                      *
      *   "mmap": { "mmap_array": [ "BatteryStatus", "LockBody" ] },           *
      *   "BatteryStatus": {                                                   *
      *     "src_core": "nRF52840",                                            *
      *     "dst_core": "PSoc6_M0",                                            *
      *     "mem_size": 12,                                                    *
      *     "mem_data": "hello world"                                          *
      *   },                                                                   *
      *   "LockBody": {                                                        *
      *     "src_core": "nRF52840",                                            *
      *     "dst_core": "PSoc6_M0",                                            *
      *     "mem_size": 8,                                                     *
      *     "mem_data": "hello world"                                          *
      *   }                                                                    *
      * }                                                                      *
      *                                                                        *
      *************************************************************************/
      
            cJSON *js = cJSON_CreateObject();
            if( js != NULL ){
                cJSON *mmap_js = cJSON_CreateObject();
                if( mmap_js != NULL ){
                    cJSON_AddItemToObject( js, "mmap", mmap_js );
                
                    cJSON *mmap_array = cJSON_CreateArray();
                    if( mmap_array == NULL ){ goto DELETE_JSON; }

                    cJSON_AddItemToObject( mmap_js, "mmap_array", mmap_array );
                    cJSON_AddItemToArray( mmap_array, cJSON_CreateString(mem_name) );
                
                    cJSON *mem_obj = cJSON_CreateObject();
                    if( mem_obj != NULL ){
                        cJSON_AddItemToObject( mmap_js, mem_name, mem_obj );

                        cJSON_AddStringToObject( mem_obj, "src_core", src_core );      // <! source from
                        cJSON_AddStringToObject( mem_obj, "dst_core", dst_core );      // <! source from
                        cJSON_AddNumberToObject( mem_obj, "mem_size", mem_size );      // <! source from
                
                        cJSON_AddItemToObject( mem_obj, (const char *)"mem_data", 
                        cJSON_CreateHexString((uint8_t *)mem_data, mem_size) );
                
                        bool ret = kernel_router_json( mcu->core, js, NULL, 0, avoid_tunnel );
                        cJSON_Delete( js );
                        return ret;
                    }
                }

                DELETE_JSON:
                cJSON_Delete(js); 
                return false;
            }
        }
        mcu = mcu->next;
    }
    return false;
}

/**
 *  @brief send shared memory message of local core
 * 
 *  @param [in] diff_sync: true: send when shared memory message has changed
 *                         false: send directly
 *  @param [out]
 *  @return 
 **/
static bool kernel_mmap_update_to(const char target_core[], bool diff_sync){
    bool ret = false;
    struct kernel_mmap_t *p = kernel_mmap_to_queue;
    while( p != NULL ){
        if( target_core != NULL ){
            if( strcmp(p->to_core, target_core) != 0x0 ){   // <! if we have target core
                p = p->next;
                continue;                                     // <! we only check the target core diff
            }
        }

        if( diff_sync ){
            if( memcmp(p->mem, p->prev_sync_mem, p->mem_size) == 0x0 ){
                p = p->next;
                continue;
            }
        }
        ret = kernel_mmap_outside( 
                    get_my_core_name(), p->to_core, p->mem_name,
                    p->mem, p->mem_size, NULL );

        if( ! ret ){ break; }
        memcpy( p->prev_sync_mem, p->mem, p->mem_size );

        p = p->next;
    }
    return ret;
}

/*************************************************************************

           -------------------------------------------------
          |                                                 |
          |         Manage core and its sub tasks           | 
          |                                                 |
           -------------------------------------------------
           
*************************************************************************/
static void kernel_delete_mcu(const char *core_name){
    struct MCUs_t *p = is_mcu_exist( core_name );
    if( p != NULL ){
        struct kernel_external_task_t *t = NULL;

        while( NULL != (t = p->task_queue) ){ UNMOUNT(p->task_queue, t); x_free(t); }
        UNMOUNT( kernel_mcu_queue, p );     x_free(p);
    }
}

static struct MCUs_t * kernel_create_mcu(const char *core_name, struct comm_tunnel_t *tunnel, int jump){
    ASSERT_NULL( core_name );
    struct MCUs_t *p = x_malloc( sizeof(struct MCUs_t) + strlen(core_name) + 1 );
    if( p != NULL ){
        memset( p, 0x0, sizeof(struct MCUs_t) + strlen(core_name) + 1 );
        p->tunnel = tunnel;
        p->jump   = jump;
        strcpy( p->core, core_name );
        MOUNT( kernel_mcu_queue, p );
    }
    return p;
}

bool kernel_change_mcu_tunnel(struct MCUs_t *mcu, struct comm_tunnel_t *tunnel, int jump){
    ASSERT_NULL( mcu );
    ASSERT_NULL( tunnel );

    mcu->tunnel = tunnel;
    mcu->jump   = jump;
    return true;
}

// !> find aimed task on aimed mcu
static struct kernel_external_task_t * kernel_is_task_on_mcu(struct MCUs_t *mcu, const char *task_name){
    ASSERT_NULL( mcu );
    ASSERT_NULL( task_name );

    struct kernel_external_task_t *t = mcu->task_queue;
    while( t != NULL ){
        if( strcmp(t->task_name, task_name) == 0x0 ){ break; }
        t = t->next;
    }
    return t;
}

static struct kernel_external_task_t * kernel_add_task_to_mcu(struct MCUs_t *mcu, const char *task_name){
    ASSERT_NULL( mcu );
    ASSERT_NULL( task_name );

    struct kernel_external_task_t *t = kernel_is_task_on_mcu( mcu, task_name );
    if( t == NULL ){
        t = (struct kernel_external_task_t *)x_malloc( sizeof(struct kernel_external_task_t) + strlen(task_name) + 1 );
        memset( t, 0x0, sizeof(struct kernel_external_task_t) + strlen(task_name) + 1 );
        strcpy( t->task_name, task_name );

        MOUNT( mcu->task_queue, t );
    }
    return t;
}

/**
 *  @brief update task list
 * 
 *  @param [in] 
 *  @param [out]
 *  @return 
 **/
bool kernel_recover_external_task_on_tunnel(struct comm_tunnel_t *tunnel, char *json){
    LOG( "%s\r\n", json );

    cJSON *js = cJSON_Parse( (char *)json );
    if( js != NULL ){
        cJSON *cores_array = cJSON_GetObjectItem( js, "Cores" );
        if( cores_array != NULL ){
            int core_num = cJSON_GetArraySize( cores_array );
            for( int i=0; i<core_num; i++ ){
                char *core_name = cJSON_GetArrayItem(cores_array, i)->valuestring;
                cJSON *core = cJSON_GetObjectItem( js, core_name );
                if( core == NULL ){ continue; }

                struct MCUs_t *mcu = is_mcu_exist( core_name );
                if( mcu == NULL ){
                    cJSON *js_jump = cJSON_GetObjectItem( core, "Jump" );
                    if( NULL != (mcu = kernel_create_mcu(core_name, tunnel,  (js_jump)?(js_jump->valueint):(1) )) ){
                        mcu->tunnel = tunnel;
                        cJSON *js_extra_support = cJSON_GetObjectItem( core, "SupportJsonExtra" );
                        if( (js_extra_support != NULL) && (mcu != NULL) ){
                            switch( js_extra_support->type ){
                                case cJSON_True : mcu->support_json_extra = true;   break;
                                default         : mcu->support_json_extra = false;  break;
                            }
                        }

                        cJSON *task_array = cJSON_GetObjectItem( core, "TaskArray" );
                        if( task_array == NULL ){ continue; }
                        int task_num = cJSON_GetArraySize( task_array );
                    
                        if( mcu != NULL ){
                            for( int n=0; n<task_num; n++ ){
                                char *task_name = cJSON_GetArrayItem(task_array, n)->valuestring;
                                if( NULL == kernel_is_task_on_mcu( mcu, task_name ) ){
                                    struct kernel_external_task_t *t = kernel_add_task_to_mcu( mcu, task_name );
                                    if( t != NULL ){ t->cached = true; }                    // <! Indentify as Cached task.!
                                }
                            }
                        } 
                    }
                }else{
                    WARNING( "mcu[%s] already existed, ignore.", mcu->core );
                    continue;                                                       // <! Ignore Task Updating
                }
            }
        }
        cJSON_Delete( js );
        return true;
    }
    return false;
}

static bool kernel_clear_cache_task_on_mcu(struct MCUs_t *mcu){
    ASSERT_NULL( mcu );                                     // <! Only Call when Recv Core Update

    struct kernel_external_task_t *t = mcu->task_queue;
    while( t != NULL ){
        if( t->cached ){                                      // <! Remove the Old cache
            mcu->task_modified = true;
            struct kernel_external_task_t *n = t->next;
            UNMOUNT( mcu->task_queue, t );
            x_free( t );  t = n;  continue;
        }
        t = t->next;
    }
}

/**
 *  @brief this api is not used
 * 
 *  @param [in] 
 *  @param [out]
 *  @return 
 **/
char * kernel_backup_external_task_on_tunnel(struct comm_tunnel_t *tunnel){
{                                                         // <! backup task list on tunnel
    struct MCUs_t *mcu = kernel_mcu_queue;                  // <! Add 2020-11-6
    while( mcu != NULL ){
        if( (!mcu->is_local) && (mcu->tunnel == tunnel) ){    // <! Filter for tunnel
            if( mcu->task_modified ){                           // <! Filter Non-Modify situation
                goto CREATE_BACKUP_FILE;
            }
        }
        mcu = mcu->next;
    }
    LOG( "Nothing has changed, no need to back up.\n" );
    return NULL;                                            // <! Return NULL when nothing changed
}

CREATE_BACKUP_FILE :
    LOG( "Creating backup file\n" );                          // <! Create backup file when any modification happened

    cJSON *js = cJSON_CreateObject();
    if( js == NULL ){ return NULL; }

    cJSON *cores_array = cJSON_CreateArray();
    if( cores_array == NULL ){ goto ERR; }

    cJSON_AddItemToObject( js, "Cores", cores_array );

  /*************************************************************************
  *                                                                        *
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |  Pack up ALL Core Task List to ALL MCUs in JSON |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *                                                                        *
  *   {                                                                    *
  *     "Cores": [ "PSoc6_M0", "PSoc6_M4", "nRF52840" ],                   *
  *     "PSoc6_M0": {                                                      *
  *       "TaskArray": [                                                   *
  *         "cap_sense_task",                                              *
  *         "NFC_task",                                                    *
  *       ]                                                                *
  *     },                                                                 *
  *     "PSoc6_M4": {                                                      *
  *       "TaskArray": [                                                   *
  *         "fps_task",                                                    *
  *         "audio_task"                                                   *
  *       ]                                                                *
  *     },                                                                 *
  *     "nRF52840": {                                                      *
  *       "TaskArray": [                                                   *
  *         "motor_task",                                                  *
  *         "lock_body_task",                                              *
  *       ]                                                                *
  *     }                                                                  *
  *   }                                                                    *
  *                                                                        *
  *************************************************************************/

    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        if( (!mcu->is_local) && (mcu->tunnel == tunnel) ){          // <! Only backup specific channel
            cJSON *core_name = cJSON_CreateString( mcu->core );
            if( core_name == NULL ){ goto ERR; }
            cJSON_AddItemToArray( cores_array, core_name );

            cJSON *core = cJSON_CreateObject();
            if( core == NULL ){ goto ERR; }
            cJSON_AddItemToObject( js, mcu->core, core );
            
            cJSON_AddNumberToObject( core , "Jump" , mcu->jump );     // <! Better to backup Jump point
            cJSON_AddBoolToObject( core , "SupportJsonExtra" , mcu->support_json_extra );
            
            cJSON *task_array = cJSON_CreateArray();
            if( task_array == NULL ){ goto ERR; }
            cJSON_AddItemToObject( core, "TaskArray", task_array );

            struct kernel_external_task_t  *task = mcu->task_queue;
            while( task != NULL ){                                      // <! Backup Task Array
                cJSON *task_item = cJSON_CreateString( task->task_name );
                if( task_item == NULL ){ goto ERR; }
                cJSON_AddItemToArray( task_array, task_item );
            
                task = task->next;
            }
        }
        mcu = mcu->next;
    }

  /*************************************************************************
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |        Convert to Json Strings for Storage      |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *************************************************************************/

    char *jsString = cJSON_PrintUnformatted( js );                // <! String Conversion
    cJSON_Delete( js );    //LOG( "%s\r\n", jsString );
    return jsString;                                              // <! return JSON String using x_malloc()

    ERR:
    WARNING( "Backup Task Failed" );
    cJSON_Delete( js );     return NULL;
}

/**
 *  @brief sync tasks and tunnels of cores
 * 
 *  @param [in] 
 *  @param [out]
 *  @return 
 **/
void synchonize_tasklist(const char local_core[], int tunnel_num, ...){

  /*************************************************************************
  *                                                                        *
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |           Update Local Core Task List           |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *                                                                        *
  *************************************************************************/

    if( local_core != NULL ){
        struct MCUs_t *mcu = is_mcu_exist( local_core );

        if( mcu == NULL ){
            mcu = kernel_create_mcu( local_core, NULL, 0 );
            if( mcu != NULL ){
                va_list args; 
                va_start( args, tunnel_num );
                for( int i=0; i<tunnel_num; i++ ){
                    struct comm_tunnel_t *tunnel = va_arg( args, struct comm_tunnel_t * );
                    MOUNT( mcu->tunnel, tunnel );
                }
                va_end( args );
        
                mcu->is_local = true;
                mcu->support_json_extra = true;
            }
        }

        if( mcu != NULL ){
            struct kernel_task_t *t = kernel_task_queue;
            while( t != NULL ){
                kernel_add_task_to_mcu( mcu, t->task_name );
                t = t->next;
            }
        }
    }
  
  /*************************************************************************
  *                                                                        *
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |  Pack up ALL Core Task List to ALL MCUs in JSON |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *                                                                        *
  *   {                                                                    *
  *     "Cores": [ "PSoc6_M0", "PSoc6_M4", "nRF52840" ],                   *
  *     "PSoc6_M0": {                                                      *
  *       "TaskArray": [                                                   *
  *         "cap_sense_task",                                              *
  *         "NFC_task",                                                    *
  *       ]                                                                *
  *     },                                                                 *
  *     "PSoc6_M4": {                                                      *
  *       "TaskArray": [                                                   *
  *         "fps_task",                                                    *
  *         "audio_task"                                                   *
  *       ]                                                                *
  *     },                                                                 *
  *     "nRF52840": {                                                      *
  *       "TaskArray": [                                                   *
  *         "motor_task",                                                  *
  *         "lock_body_task",                                              *
  *       ]                                                                *
  *     }                                                                  *
  *   }                                                                    *
  *                                                                        *
  *************************************************************************/

    cJSON *js = cJSON_CreateObject();
    if( js == NULL ){ return; }

    cJSON *cores_array = cJSON_CreateArray();
    if( cores_array == NULL ){ goto ERR; }

    cJSON_AddItemToObject( js, "Cores", cores_array );

    struct MCUs_t *mcu = kernel_mcu_queue;  int mcu_num = 0;
    while( mcu != NULL ){
        if( ! mcu->is_local ){                                          // <! Check Non-local MCU
            struct kernel_external_task_t  *t = mcu->task_queue;
            while( t != NULL ){ if( t->cached ){ break; } t = t->next; }  // <! Remove cached tasks
            if( t != NULL ){ mcu = mcu->next; continue; }                 // <! Ignore MCU when cached
        }                                                               // <! Add 2020-11-6

        cJSON *core_name = cJSON_CreateString( mcu->core );
        if( core_name == NULL ){ goto ERR; }
        cJSON_AddItemToArray( cores_array, core_name );

        cJSON *core = cJSON_CreateObject();
        if( core == NULL ){ goto ERR; }
        cJSON_AddItemToObject( js, mcu->core, core );

        cJSON_AddNumberToObject( core , "Jump" , mcu->jump + 1 );

        cJSON_AddBoolToObject( core , "SupportJsonExtra" , mcu->support_json_extra );

        cJSON *task_array = cJSON_CreateArray();
        if( task_array == NULL ){ goto ERR; }
        cJSON_AddItemToObject( core, "TaskArray", task_array );

        struct kernel_external_task_t  *task = mcu->task_queue;
        while( task != NULL ){
            cJSON *task_item = cJSON_CreateString( task->task_name );
            if( task_item == NULL ){ goto ERR; }
            cJSON_AddItemToArray( task_array, task_item );
            task = task->next;
        }
        mcu = mcu->next;
        mcu_num++;
    }

  /*************************************************************************
  *                                                                        *
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |    Send out JSON to ALL Cores  by each Tunnel   |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *                                                                        *
  *************************************************************************/

    char *jsString = cJSON_PrintUnformatted( js );
    if( jsString != NULL ){
        cJSON_Delete( js );    //LOG( "%s\r\n", jsString );

        mcu = kernel_mcu_queue;
        while( mcu != NULL ){
            if( mcu->is_local ){
                struct comm_tunnel_t *tunnel = mcu->tunnel;
                while( tunnel != NULL ){
                    if( (!tunnel->passive_tunnel) || (tunnel->passive_tunnel & tunnel->tunnel_enabled) ){
                        layer_proc_func_list *proc = tunnel->send_proc;
                
                        char *dup_jsString = __strdup__( jsString );
                        if( dup_jsString != NULL ){
                            int32_t sent_length = proc[0].func( &proc[1], tunnel, (uint8_t *)dup_jsString, strlen(jsString) + 1 );
                        }
                    }
                    tunnel = tunnel->next;
                } break;
            }
            mcu = mcu->next;
        }
        x_free( jsString );
        return;
    }

ERR:
    WARNING( "Synchonize Task Failed" );
    cJSON_Delete( js );     return;
}

/**
 *  @brief post msg to other cores
 * 
 *  @param [in] 
 *  @param [out]
 *  @return 
 **/
static bool try_post_msg_outside(const char *target_task, struct kernel_msg_t *msg, const char *src_task){
    ASSERT_NULL( target_task );
    ASSERT_NULL( msg );
    if( msg == NULL ){ return false; }
    if( target_task == NULL ){ return false; }
  
  /*************************************************************************
  *                                                                        *
  *          -------------------------------------------------             *
  *         |                                                 |            *
  *         |   Try to Post msg outside of this MCU  in JSON  |            *
  *         |                                                 |            *
  *          -------------------------------------------------             *
  *                                                                        *
  *   {                                                                    *
  *     "msg": {                                                           *
  *       "targ_task": "audio_task",                                       *
  *       "notify": "audio_play",                                          *
  *       "data": "wakeing.lst"                                            *
  *     }                                                                  *
  *   }                                                                    *
  *                                                                        *
  *************************************************************************/
    struct MCUs_t *mcu = kernel_mcu_queue;
    while( mcu != NULL ){
        struct kernel_external_task_t *t = kernel_is_task_on_mcu( mcu, target_task );
        if( t != NULL ){
            LOG( "Found [%s] on core[%s], try post msg\r\n", t->task_name, mcu->core );
        
            bool support_json_extra = all_support_json_extra;
        
            kernel_mmap_update_to( mcu->core, true );                                     // <! update mmap before post msg

            cJSON *js = cJSON_CreateObject();
            if( js != NULL ){
                cJSON *msg_js = cJSON_CreateObject();
                if( msg_js != NULL ){
                    cJSON_AddItemToObject( js, "msg", msg_js );

                    cJSON_AddStringToObject( msg_js, "targ_task", t->task_name );             // <! target task name
                    if( msg->msg.notification != NULL ){
                        cJSON_AddStringToObject( msg_js, "notify", msg->msg.notification );     // <! notification
                    }
                    if( msg->msg.length > 0 ){
                        if( str_verify(msg->msg.data, msg->msg.length) == msg->msg.length ){    // <! this is string
                            cJSON_AddStringToObject( msg_js, "data", msg->msg.data );     
                        }else{                                                                  // <! this is hex data
                            if( support_json_extra ){
                                cJSON_AddItemToObject( msg_js, (const char *)"data", cJSON_CreateHexString(NULL, 0) );
                            }else{
                                cJSON_AddItemToObject( msg_js, (const char *)"data", cJSON_CreateHexString((uint8_t *)msg->msg.data, msg->msg.length) );
                            }
                        }
                    }
                    if( src_task != NULL ){
                        cJSON_AddStringToObject( msg_js, "src_task", src_task );                // <! source task name
                    }
            
                    if( msg->timer.enable ){
                        cJSON_AddStringToObject( msg_js, "timer", "enable" );                   // <! timer
            
                        cJSON_AddNumberToObject( msg_js, "delay", msg->timer.delay );           // <! timer delay time
                        cJSON_AddNumberToObject( msg_js, "preodic", msg->timer.preodic );       // <! timer preodic call time
                        cJSON_AddNumberToObject( msg_js, "cnt", msg->timer.cnt );               // <! timer preodic call count
                    }
                }
        
                bool ret = false;
                if( support_json_extra ){ ret = kernel_router_json( mcu->core, js, msg->msg.data, msg->msg.length, NULL ); }
                else                    { ret = kernel_router_json( mcu->core, js, NULL,          0,               NULL ); }
        
                cJSON_Delete( js );
                return ret;
            }
            break;
        }
        mcu = mcu->next;
    }

    /*************************************************************************
    *                                                                        *
    *          -------------------------------------------------             *
    *         |                                                 |            *
    *         |   Could not find task on any MCU, try re-sync   |            *
    *         |                                                 |            *
    *          -------------------------------------------------             *
    *                                                                        *
    *************************************************************************/

    //  LOG( "[%s] not found, try re-sync\n", target_task );
    //  synchonize_tasklist( NULL, 0 );
    return false;
}

int32_t kernel_msg_layer_unpack(layer_proc_func_list *proc, void *arg, uint8_t *data, int32_t length){

    if( data == NULL ){ return length; }
    if( length == 0 ) { return length; }
    if( proc == NULL ){ return length; }
    struct comm_tunnel_t *tunnel = (struct comm_tunnel_t *)arg;

    uint8_t *raw_data = data;
    cJSON *js = cJSON_Parse( (char *)data );
    if( js != NULL ){

    /*************************************************************************
    *                                                                        *
    *          -------------------------------------------------             *
    *         |                                                 |            *
    *         |     Try to receive kernel message  From JSON    |            *
    *         |                                                 |            *
    *          -------------------------------------------------             *
    *                                                                        *
    *   {                                                                    *
    *     "msg": {                                                           *
    *       "targ_task": "audio_task",                                       *
    *       "notify": "audio_play",                                          *
    *       "data": "wakeing.lst"                                            *
    *     }                                                                  *
    *   }                                                                    *
    *                                                                        *
    *************************************************************************/
        cJSON *msg_js = cJSON_GetObjectItem( js, "msg" );
        if( msg_js != NULL ){
            xMsgHandler kmsg = NULL;
            char *target_task = NULL, *notification = NULL, *data = NULL, *src_task = NULL;   cJSON *o = NULL;

            if( NULL != (o = cJSON_GetObjectItem(msg_js, "targ_task")) ){ target_task = o->valuestring;  }
            if( NULL != (o = cJSON_GetObjectItem(msg_js, "src_task")) ) { src_task = o->valuestring;     }
            if( NULL != (o = cJSON_GetObjectItem(msg_js, "notify")) )   { notification = o->valuestring; }
            if( NULL != (o = cJSON_GetObjectItem(msg_js, "data")) )     { data = o->valuestring;         }
      
            struct MCUs_t *dst_mcu = kernel_mcu_queue;
            while( dst_mcu != NULL ){
                struct kernel_external_task_t *t = kernel_is_task_on_mcu( dst_mcu, target_task );
                if( t != NULL ){ break; }
                dst_mcu = dst_mcu->next;
            }
            if( dst_mcu != NULL ){
                if( dst_mcu->is_local ){                // <! local mcu, post locally
                    if( o == NULL ){
                        kmsg = new_msg(notification);
                        //post_msg( target_task, new_msg(notification) );
                        //LOG( "post_msg(%s, new_msg(%s));\r\n", target_task, notification );
                    }else{
                        switch( o->type ){
                            case cJSON_String    :  
                                if( data != NULL ){
                                    kmsg = new_msg(notification, data);
                                        //post_msg( target_task, new_msg(notification, data) );
                                        //LOG( "post_msg(%s, new_msg(%s, %s));\r\n", target_task, notification, data );
                                    } break;
              
                            case cJSON_HexString :  ;
                                int32_t len  = o->valueint;
                                if( len == 0 ){                                             // <! Extra data carry ouside of JSON.
                                    int extra_len = length - ( strlen((char *)raw_data) + 1 );        // <! Measure extra length
                                    if( extra_len >= (int)sizeof(struct json_extra_data_t) ){       // <! Length Validation
                                        struct json_extra_data_t *ex = (struct json_extra_data_t *)&raw_data[ strlen((char *)raw_data) + 1 ];
                                        if( ex->data_type == cJSON_HexString ){                 // <! Data Type Confirmation
                                            if( ex->length <= extra_len - (sizeof(struct json_extra_data_t) - sizeof(((struct json_extra_data_t *)0)->data)) ){
                                                data = (char *)ex->data;                                    // <! Data Extration
                                                len  = ex->length;
                                            }
                                        }
                                    }
                                }else{
                                    data = (char *)cJSON_hexassemble( o->valuestring );       // <! Hex Converted Data Method
                                }
                                if( (data != NULL) && (len > 0) ){
                                    kmsg = new_msg(notification, data, len);
                                    //LOG( "post_msg(%s, new_msg(%s, %s, %d));\r\n", target_task, notification, data, len );
                                    if( o->valueint > 0 ){                                    // <! Hex Converted Data Method
                                        x_free( data );
                                    }
                                } break;
                        }
                    }
          
                    if( NULL != (o = cJSON_GetObjectItem(msg_js, "timer")) ){
                        int32_t delay = 0, preodic = -1, cnt = -1;
                        if( NULL != (o = cJSON_GetObjectItem(msg_js, "delay")) )  { delay = o->valueint;   }
                        if( NULL != (o = cJSON_GetObjectItem(msg_js, "preodic")) ){ preodic = o->valueint; }
                        if( NULL != (o = cJSON_GetObjectItem(msg_js, "cnt")) )    { cnt = o->valueint;     }
            
                        if( kmsg != NULL ){
                            kmsg = msg_set_timer( kmsg, delay, preodic, cnt );
                        }
                    }
                    if( kmsg != NULL ){
                        post_msg( target_task, kmsg, src_task );
                    }
                }else{                                      // <! non local task, router outside
                    if( is_tunnel_available(kernel_aquire_tunnel_by_core(dst_mcu->core), dst_mcu->core, target_task, notification) ){
                        uint8_t *router_data = (uint8_t *)x_malloc( length );
                        if( router_data != NULL ){
                            memcpy( router_data, raw_data, length );
                            kernel_router_raw( dst_mcu->core, router_data, length, tunnel );     // <! Router Total Msg
                        }
                    }
                }
            } 
        }
    
        const char *local_core = get_my_core_name();
        if( local_core != NULL ){
      
      /*************************************************************************
      *                                                                        *
      *          -------------------------------------------------             *
      *         |                                                 |            *
      *         |   Try to Sync kernel memory mapping  From JSON  |            *
      *         |                                                 |            *
      *          -------------------------------------------------             *
      *                                                                        *
      * {                                                                      *
      *   "mmap": { "mmap_array": [ "BatteryStatus", "LockBody" ] },           *
      *   "BatteryStatus": {                                                   *
      *     "src_core": "nRF52840",                                            *
      *     "dst_core": "PSoc6_M0",                                            *
      *     "mem_size": 12,                                                    *
      *     "mem_data": "hello world"                                          *
      *   },                                                                   *
      *   "LockBody": {                                                        *
      *     "src_core": "nRF52840",                                            *
      *     "dst_core": "PSoc6_M0",                                            *
      *     "mem_size": 8,                                                     *
      *     "mem_data": "hello world"                                          *
      *   }                                                                    *
      * }                                                                      *
      *                                                                        *
      *************************************************************************/
      
            cJSON *mmap_js = cJSON_GetObjectItem( js, "mmap" );
            if( mmap_js ){
                cJSON *mmap_array = cJSON_GetObjectItem( mmap_js, "mmap_array" );
                if( mmap_array != NULL ){
                    int mmap_num = cJSON_GetArraySize( mmap_array );
            
                    for( int i=0; i<mmap_num; i++ ){
                        char *mem_name = cJSON_GetArrayItem(mmap_array, i)->valuestring;
                        cJSON *mem_obj = cJSON_GetObjectItem( mmap_js, mem_name );
                        if( mem_obj == NULL ){ continue; }
                
                        char *src_core = NULL, *dst_core = NULL;    cJSON *o = NULL;
                        int mem_size = 0;
                
                        if( NULL != (o = cJSON_GetObjectItem(mem_obj, "src_core")) ){ src_core = o->valuestring; }
                        if( NULL != (o = cJSON_GetObjectItem(mem_obj, "dst_core")) ){ dst_core = o->valuestring; }
                        if( NULL != (o = cJSON_GetObjectItem(mem_obj, "mem_size")) ){ mem_size = o->valueint;    }
                
                        char *mem_data = NULL;
                        if( NULL != (o = cJSON_GetObjectItem(mem_obj, "mem_data")) ){ mem_data = o->valuestring; }
                        switch( o->type ){
                            case cJSON_HexString :  
                                mem_data = (char *)cJSON_hexassemble( o->valuestring );
                                int32_t len  = o->valueint;
                                if( (mem_data == NULL) || (len <= 0) ){
                                    WARNING( "mem_data NULL or length <= 0" );  continue;
                                }
                                if( len != mem_size ){
                                    WARNING( "mmap size not match" );  continue;
                                } break;
                            default :  
                                WARNING( "mmap type Error!!!" );   continue;
                        }
                
                        if( strcmp(dst_core, local_core) == 0x0 ){      // <! destination is me
                            kernel_mmap_update_from( src_core, mem_name, mem_data, mem_size );
                            LOG( "mmap update from (%s), %s[%d]\r\n", src_core, mem_name, mem_size );
                        }else{                                        // <! Destination is not me, pass it on
                            kernel_mmap_outside( src_core, dst_core, mem_name, mem_data, mem_size, tunnel );
                        }
                        x_free( mem_data );
                    }
                }
            }
        
        /*************************************************************************
         *                                                                        *
        *          -------------------------------------------------             *
        *         |                                                 |            *
        *         |        Process mmap request  From JSON          |            *
        *         |                                                 |            *
        *          -------------------------------------------------             *
        * mmap_sync_req                                                          *
        * {                                                                      *
        *     "src_core": "nRF52840",                                            *
        *     "dst_core": "PSoc6_M0",                                            *
        * }                                                                      *
        *                                                                        *
        *************************************************************************/
        
            cJSON *mmap_req = cJSON_GetObjectItem( js, "mmap_sync_req" );
            if( mmap_req ){
                char *src_core = NULL, *dst_core = NULL;    cJSON *o = NULL;
            
                if( NULL != (o = cJSON_GetObjectItem(mmap_req, "src_core")) ){ src_core = o->valuestring; }
                if( NULL != (o = cJSON_GetObjectItem(mmap_req, "dst_core")) ){ dst_core = o->valuestring; }
            
                if( (dst_core != NULL) && (src_core != NULL) ){
                    if( strcmp(src_core, get_my_core_name()) == 0x0 ){
                        kernel_mmap_update_to( dst_core, false );
                    }else{
                        kernel_mmap_request( src_core, dst_core, tunnel );
                    }
                }
            }
        
        /*************************************************************************
         *                                                                        *
        *          -------------------------------------------------             *
        *         |                                                 |            *
        *         |    Try to Synchonize Task List From ALL MCUs    |            *
        *         |                                                 |            *
        *          -------------------------------------------------             *
        *                                                                        *
        *   {                                                                    *
        *     "Cores": [ "PSoc6_M0", "PSoc6_M4", "nRF52840" ],                   *
        *     "PSoc6_M0": {                                                      *
        *       "TaskArray": [                                                   *
        *         "cap_sense_task",                                              *
        *         "NFC_task",                                                    *
        *       ]                                                                *
        *     },                                                                 *
        *     "PSoc6_M4": {                                                      *
        *       "TaskArray": [                                                   *
        *         "fps_task",                                                    *
        *         "audio_task"                                                   *
        *       ]                                                                *
        *     },                                                                 *
        *     "nRF52840": {                                                      *
        *       "TaskArray": [                                                   *
        *         "motor_task",                                                  *
        *         "lock_body_task",                                              *
        *       ]                                                                *
        *     }                                                                  *
        *   }                                                                    *
        *                                                                        *
        *************************************************************************/
        
            bool list_changed = false;
            uint32_t peer_sum = 0;
        
            cJSON *cores_array = cJSON_GetObjectItem( js, "Cores" );
            if( cores_array != NULL ){
                int core_num = cJSON_GetArraySize( cores_array );
                for( int i=0; i<core_num; i++ ){
            
                    char *core_name = cJSON_GetArrayItem(cores_array, i)->valuestring;
                    cJSON *core = cJSON_GetObjectItem( js, core_name );
                    if( core == NULL ){ continue; }
                    str_chksum( &peer_sum, core_name );

                    cJSON *task_array = cJSON_GetObjectItem( core, "TaskArray" );
                    if( task_array == NULL ){ continue; }
                    int task_num = cJSON_GetArraySize( task_array );
            
                    cJSON *js_jump = cJSON_GetObjectItem( core, "Jump" );
            
                    struct MCUs_t *mcu = is_mcu_exist( core_name );
                    if( mcu == NULL ){
                        if( NULL != (mcu = kernel_create_mcu(core_name, tunnel,  (js_jump)?(js_jump->valueint):(1) )) ){
                            kernel_mmap_update_to( core_name, false );       // <! update mmap when core created
                            mcu->task_modified = true;
                            list_changed = true;
                        }
                    }else{
                        if( js_jump != NULL ){
                            int jump = js_jump->valueint;
                            if( jump < mcu->jump ){
                                if( kernel_change_mcu_tunnel(mcu, tunnel, jump) ){
                                    kernel_mmap_update_to( core_name, false );   // <! update mmap when tunnel changed
                                    mcu->task_modified = true;
                                    list_changed = true;
                                }
                            }
                        }
                    }
            
                    cJSON *js_extra_support = cJSON_GetObjectItem( core, "SupportJsonExtra" );
                    if( (js_extra_support != NULL) && (mcu != NULL) ){
                        switch( js_extra_support->type ){
                            case cJSON_True : mcu->support_json_extra = true;   break;
                            default         : mcu->support_json_extra = false;  break;
                        }
                    }

                    if( mcu != NULL ){
                        for( int n=0; n<task_num; n++ ){
                            char *task_name = cJSON_GetArrayItem(task_array, n)->valuestring;
                            struct kernel_external_task_t *task = NULL;
                            if( NULL == (task = kernel_is_task_on_mcu(mcu, task_name)) ){
                                task = kernel_add_task_to_mcu( mcu, task_name );
                                mcu->task_modified = true;
                                list_changed = true;
                            }
                            if( task != NULL ){ task->cached = false; }       // <! if Task Existed or Newly created, Mark as non-cached
                            str_chksum( &peer_sum, task_name );
                        }
                        kernel_clear_cache_task_on_mcu( mcu );
                    }
                }
            
                if( peer_sum != get_local_sync_list_chksum() ){
                    list_changed = true;
                }
            
                if( list_changed ){
                    LOG( "list %s\r\n", (list_changed)?"changed":"unchange" );
                    synchonize_tasklist( NULL, 0 );
                    kernel_mmap_check_unsync_core( 300 );                 // <! when list_changed, check unsync after 300ms
                }
            
                {
                bool is_all_support = true;
                struct MCUs_t *p = kernel_mcu_queue;                  // <! Check if ALL support json extra_data
                while( p != NULL ){
                    if( ! p->support_json_extra ){
                        is_all_support = false;
                    } p = p->next;
                }
                all_support_json_extra = is_all_support;
                }
            
                draw_topo_layer( kernel_mcu_queue, 0 );
            }

            cJSON_Delete( js );
        }
    }
    return 0;
}



