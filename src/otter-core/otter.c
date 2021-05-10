#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>         // gethostname
#include <sys/time.h>       // getrusage
#include <sys/resource.h>   // getrusage

#include <otf2/otf2.h>

#include <macros/debug.h>
#include <macros/callback.h>

#include <otter-ompt-header.h>

#include <otter-common.h>
#include <otter-core/otter.h>
#include <otter-core/otter-structs.h>
#include <otter-core/otter-entry.h>
#include <otter-core/otter-environment-variables.h>
#include <otter-datatypes/graph.h>
#include <otter-task-graph/task-graph.h>
#include <otter-trace/trace.h>

/* Static function prototypes */
static void print_resource_usage(void);

/* Callback used by task-graph to allow otter to control destruction of graph
   node data */
static void destroy_graph_node_data(
    void *node_data, graph_node_type_t node_type);

/* OMPT entrypoint signatures */
ompt_get_thread_data_t     get_thread_data;
ompt_get_parallel_info_t   get_parallel_info;

/* Register the tool's callbacks with otter-entry which will pass them on to OMP
*/
void
tool_setup(
    tool_callbacks_t        *callbacks,
    ompt_function_lookup_t  lookup)
{
    include_callback(callbacks, ompt_callback_parallel_begin);
    include_callback(callbacks, ompt_callback_parallel_end);
    include_callback(callbacks, ompt_callback_thread_begin);
    include_callback(callbacks, ompt_callback_thread_end);
    include_callback(callbacks, ompt_callback_task_create);
    // include_callback(callbacks, ompt_callback_task_schedule);
    include_callback(callbacks, ompt_callback_implicit_task);
    include_callback(callbacks, ompt_callback_work);
    include_callback(callbacks, ompt_callback_sync_region);

    get_thread_data = (ompt_get_thread_data_t) lookup("ompt_get_thread_data");
    get_parallel_info = 
        (ompt_get_parallel_info_t) lookup("ompt_get_parallel_info");

    static char host[HOST_NAME_MAX+1] = {0};
    gethostname(host, HOST_NAME_MAX);

    /* detect environment variables for graph output file */
    static otter_opt_t opt = {
        .hostname         = NULL,
        .graph_output     = NULL,
        .graph_format     = NULL,
        .graph_nodeattr   = NULL,
        .append_hostname  = false
    };

    opt.hostname = host;
    opt.graph_output = getenv(ENV_VAR_GRAPH_FILE);
    opt.graph_format = getenv(ENV_VAR_GRAPH_FMT);
    opt.graph_nodeattr = getenv(ENV_VAR_NODE_ATTR_FILE);
    opt.append_hostname = 
        getenv(ENV_VAR_APPEND_HOST) == NULL ? false : true;

    LOG_INFO("Otter environment variables:");
    LOG_INFO("%-30s %s","host", opt.hostname);
    LOG_INFO("%-30s %s",ENV_VAR_GRAPH_FILE, opt.graph_output);
    LOG_INFO("%-30s %s",ENV_VAR_GRAPH_FMT, opt.graph_format);
    LOG_INFO("%-30s %s",ENV_VAR_NODE_ATTR_FILE, opt.graph_nodeattr);
    LOG_INFO("%-30s %s",ENV_VAR_APPEND_HOST,opt.append_hostname?"Yes":"No");

    // task_graph_init(&opt);
    trace_initialise_archive(&opt);

    return;
}

/* Call appropriate destructor for the different kinds of data referenced by 
   task graph nodes */
static void
destroy_graph_node_data(
    void               *node_data,
    graph_node_type_t   node_type)
{
    LOG_DEBUG("(0x%x) %p", node_type, node_data);

    switch (node_type)
    {
        case node_task_initial:
        case node_task_implicit:
        case node_task_explicit:
        case node_task_target:
            task_destroy((task_data_t*) node_data);
            break;
        case node_scope_parallel_begin:
            parallel_destroy((parallel_data_t*) node_data);
            break;
        default:
            break;
    }
    return;
}

void
tool_finalise(void)
{
    // task_graph_write();
    // task_graph_destroy(&destroy_graph_node_data);
    trace_finalise_archive();
    print_resource_usage();
    return;
}

static void
print_resource_usage(void)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    #define PRINT_RUSAGE(key, val, units)\
        fprintf(stderr, "%35s: %8lu %s\n", key, usage.val, units);
    fprintf(stderr, "\nPROCESS RESOURCE USAGE:\n");
    PRINT_RUSAGE("maximum resident set size", ru_maxrss, "kb");
    PRINT_RUSAGE("page reclaims (soft page faults)", ru_minflt, "");
    PRINT_RUSAGE("page faults (hard page faults)", ru_majflt, "");
    PRINT_RUSAGE("block input operations", ru_inblock, "");
    PRINT_RUSAGE("block output operations", ru_oublock, "");
    #undef PRINT_RUSAGE

    fprintf(stderr, "\n%35s: %8lu %s\n", "threads",
        get_unique_thread_id(), "");
    fprintf(stderr, "%35s: %8lu %s\n", "parallel regions",
        get_unique_parallel_id(), "");
    fprintf(stderr, "%35s: %8lu %s\n", "tasks",
        get_unique_task_id()-1, "");
}

/* 

   Events:
   initial-thread-begin

 */
static void
on_ompt_callback_thread_begin(
    ompt_thread_t            thread_type,
    ompt_data_t             *thread)
{   
    thread_data_t *thread_data = new_thread_data(thread_type);
    thread->ptr = thread_data;

    thread_data->location = trace_new_location_definition(
        thread_data->id,
        thread_type,
        OTF2_LOCATION_TYPE_CPU_THREAD,
        DEFAULT_LOCATION_GRP);

    // trace_event_thread_begin(thread_data->location);
    trace_event_thread(thread_data->location, ompt_scope_begin);

    LOG_DEBUG_THREAD_TYPE(thread_type, thread_data->id);
    return;
}

/* 
   
   Events:
   initial-thread-end

   A thread dispatches a registered ompt_callback_thread_end callback for the
   initial-thread-end event in that thread. The callback occurs in the scope
   of the thread. The callback has type signature ompt_callback_thread_end_t.
   The implicit parallel region does not dispatch a ompt_callback_parallel_end
   callback; however, the implicit parallel region can be finalized within this
   ompt_callback_thread_end callback. 

 */
static void
on_ompt_callback_thread_end(
    ompt_data_t             *thread)
{
    thread_data_t *thread_data = thread->ptr;
    LOG_DEBUG_THREAD_TYPE(thread_data->type, thread_data->id);
    trace_event_thread(thread_data->location, ompt_scope_end);

    if (thread_data->type == ompt_thread_initial)
    {
        LOG_DEBUG("final clean-up...");
    }

    thread_destroy(thread_data);

    return;
}

/* 
   
   implicit parallel region: a parallel region, executed by one thread, not 
    generated by a parallel construct. They surround the whole OpenMP program,
    all target regions and all teams regions.

   Events:
   parallel-begin

 */
static void
on_ompt_callback_parallel_begin(
    ompt_data_t             *encountering_task,
    const ompt_frame_t      *encountering_task_frame,
    ompt_data_t             *parallel,
    unsigned int             requested_parallelism,
    int                      flags,
    const void              *codeptr_ra)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;
    task_data_t *task_data = (task_data_t*) encountering_task->ptr;

    LOG_DEBUG("[t=%lu] %-6s %s", thread_data->id, "begin", "parallel");

    thread_data->is_master_thread = true;

    /* assign space for this parallel region */
    parallel_data_t *parallel_data = new_parallel_data(flags);
    parallel->ptr = parallel_data;

    /* record enter region event */
    parallel_data->region = trace_new_parallel_region(
        parallel_data->id, thread_data->id, flags, requested_parallelism);
    trace_event(thread_data->location, parallel_data->region, ompt_scope_begin);
    // queue_push(thread_data->parallel_rgn_queue,
    //     (data_item_t) {.ptr = parallel_data->region});

    return;
}

/* 

   Events:
   parallel-end

   Order of related events:
   implicit-task-end
   parallel-region-end

 */
static void
on_ompt_callback_parallel_end(
    ompt_data_t *parallel,
    ompt_data_t *encountering_task,
    int          flags,
    const void  *codeptr_ra)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;

    scope_t *scope = NULL;

    if ((parallel == NULL) || (parallel->ptr == NULL))
    {
        LOG_ERROR("parallel end: null pointer");
    } else {
        LOG_DEBUG("[t=%lu] %-6s %s", thread_data->id, "end", "parallel");
        parallel_data_t *parallel_data = parallel->ptr;
        trace_event(thread_data->location, NULL, ompt_scope_end);
        // trace_destroy_parallel_region(parallel_data->region);
        /* reset flag */
        thread_data->is_master_thread = false;
    }

    return;
}


/* p 467

   Used for callbacks that are dispatched when taskregions or initial tasks are
   generated

   encountering_task, encountering_task_frame are NULL for an initial task

   flags:

        typedef enum ompt_task_flag_t {
            ompt_task_initial    = 0x00000001,
            ompt_task_implicit   = 0x00000002,
            ompt_task_explicit   = 0x00000004,
            ompt_task_target     = 0x00000008,
            ompt_task_undeferred = 0x08000000,
            ompt_task_untied     = 0x10000000,
            ompt_task_final      = 0x20000000,
            ompt_task_mergeable  = 0x40000000,
            ompt_task_merged     = 0x80000000
        } ompt_task_flag_t;

   explicit task: any task that is not an implicit task

   implicit task: a task generated by an implicit parallel region or when a 
    parallel construct is encountered

   initial task: a type of implicit task associated with an implicit parallel
    region

   Events:
    task-create

   task generating constructs:
    task (create a task from a region)
    taskloop (create tasks from loop iterations)
    target (generates a target task)
    target update (generates a target task)
    target enter/exit data (generates a target task)
 */
static void
on_ompt_callback_task_create(
    ompt_data_t         *encountering_task,
    const ompt_frame_t  *encountering_task_frame,
    ompt_data_t         *new_task,
    int                  flags,
    int                  has_dependences,
    const void          *codeptr_ra)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;

    /* Intel runtime seems to give the initial task a task-create event while
       LLVM just gives it an implicit-task-begin event. If compiling with Intel,
       defer initial task node creation until the implicit-task-begin event for
       simplicity */
    #if defined(__INTEL_COMPILER)
    if (flags & ompt_task_initial)
    {
        LOG_DEBUG("Intel detected -> defer intial task node creation until "
                  "implicit-task-begin");
        return;
    }
    #endif

    /* make space for the newly-created task */
    task_data_t *task_data = new_task_data(get_unique_task_id(), flags);
    new_task->ptr = task_data;

    /* get the task data of the parent, if it exists */
    task_data_t *parent_task_data = NULL;

    if (encountering_task != NULL) // only for initial tasks
    {
        parent_task_data = (task_data_t*) encountering_task->ptr;

        LOG_DEBUG_TASK_TYPE(
            thread_data->id, parent_task_data->id, task_data->id, flags);

        if (parent_task_data->type == ompt_task_implicit)
        {

        } else {

        }

    }
            
    return;
}

static void
on_ompt_callback_task_schedule(
    ompt_data_t             *prior_task,
    ompt_task_status_t       prior_task_status,
    ompt_data_t             *next_task)
{
    LOG_DEBUG_PRIOR_TASK_STATUS(prior_task_status);

    task_data_t *prior_task_data = NULL, *next_task_data = NULL;

    #if DEBUG_LEVEL >= 3
    prior_task_data = (task_data_t*) prior_task->ptr;
    next_task_data = (task_data_t*) next_task->ptr;
    LOG_DEBUG("%lu, %lu", prior_task_data->id, next_task_data->id);
    #endif
    
    return;
}

/* 

   Events:
   initial-task-begin/initial-task-end
   implicit-task-begin/implicit-task-end

 */
static void
on_ompt_callback_implicit_task(
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    unsigned int             actual_parallelism,
    unsigned int             index,
    int                      flags)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;

    task_data_t *task_data = NULL;
    parallel_data_t *parallel_data = NULL;

    if (endpoint == ompt_scope_begin)
    {   
        /* Check whether task data is null */
        // LOG_DEBUG("task pointer: %p->%p (flags=%d)", task, task->ptr, flags);

        /* Intel's runtime gives initial tasks task-create & implicit
         * -task-begin callbacks, but LLVM's only gives ITB callback
         * 
         * This means when using Intel runtime I allocate initial task
         * space in task-create, but with LLVM this happens below - need
         * to check for this to avoid double-counting an initial task
         */
        if (task->ptr != NULL)
        {
			LOG_WARN("task was previously allocated task data");
			task_data = (task_data_t*) task->ptr;
		} else {			
			/* make space for this initial or implicit task */
			task_data = new_task_data(get_unique_task_id(), flags);
            task->ptr = task_data;
		}

		LOG_DEBUG_IMPLICIT_TASK(flags, "begin", task_data->id);

        if (task_data->type == ompt_task_initial)
        {

        } else if (task_data->type == ompt_task_implicit) {

            /* Begin implicit task */

            thread_data->actual_parallelism = actual_parallelism;
            thread_data->index = index;
            parallel_data = (parallel_data_t*) parallel->ptr;
            if (!thread_data->is_master_thread)
            {   
                /* Master thread records event during parallel-begin callback */
                trace_event(thread_data->location, parallel_data->region,
                    ompt_scope_begin);
            }
            
            if (thread_data->is_master_thread)
                parallel_data->actual_parallelism = actual_parallelism;
        } else {
            // Think this shouldn't happen
            fprintf(stderr, "UNEXPECTED IMPLICIT TASK CALLBACK");
            fprintf(stderr, "flags=%d actual_parallelism=%u index=%u",
                flags, actual_parallelism, index);
            abort();
        }

    } else { /* ompt_scope_end */

        if (flags & ompt_task_implicit)
        {
            /* End implicit task */

		    LOG_DEBUG_IMPLICIT_TASK(flags, "end", ~(0L));
            thread_data->actual_parallelism = thread_data->index = 0;

            /* Worker threads record parallel-end during implicit-task-end
               callback */
            if (!thread_data->is_master_thread)
                trace_event(thread_data->location, NULL, ompt_scope_end);
        }

    }

    return;
}

static void
on_ompt_callback_target(
    ompt_target_t            kind,
    ompt_scope_endpoint_t    endpoint,
    int                      device_num,
    ompt_data_t             *task,
    ompt_id_t                target_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_target_data_op(
    ompt_id_t                target_id,
    ompt_id_t                host_op_id,
    ompt_target_data_op_t    optype,
    void                    *src_addr,
    int                      src_device_num,
    void                    *dest_addr,
    int                      dest_device_num,
    size_t                   bytes,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_target_submit(
    ompt_id_t                target_id,
    ompt_id_t                host_op_id,
    unsigned int             requested_num_teams)
{
    return;
}

static void
on_ompt_callback_device_initialize(
    int                      device_num,
    const char              *type,
    ompt_device_t           *device,
    ompt_function_lookup_t   lookup,
    const char              *documentation)
{
    return;
}

static void
on_ompt_callback_device_finalize(
    int                      device_num)
{
    return;
}

static void
on_ompt_callback_device_load(
    int                      device_num,
    const char              *filename,
    int64_t                  offset_in_file,
    void                    *vma_in_file,
    size_t                   bytes,
    void                    *host_addr,
    void                    *device_addr,
    uint64_t                 module_id)
{
    return;
}

static void
on_ompt_callback_device_unload(
    int                      device_num,
    uint64_t                 module_id)
{
    return;
}

static void
on_ompt_callback_sync_region_wait(
    ompt_sync_region_t       kind,
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_mutex_released(
    ompt_mutex_t             kind,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_dependences(
    ompt_data_t             *task,
    const ompt_dependence_t *deps,
    int                      ndeps)
{
    return;
}

static void
on_ompt_callback_task_dependence(
    ompt_data_t             *src_task,
    ompt_data_t             *sink_task)
{
    return;
}

/* Used for callbacks that are dispatched when worksharing regions, loop-related
   regions, and taskloopregions begin and end.

   Events:
    section-begin/end (scope=implicit task)
    single-begin/end
    workshare-begin/end (scope=implicit task)
    ws-loop-begin/end (scope=implicit task)
    distribute-begin/end (scope=implicit task)
    taskloop-begin/end (scope=encountering task)

   Workshare types that get new nesting scopes:
    - ompt_work_loop
    - ompt_work_sections
    - ompt_work_single_executor
    - ompt_work_distribute
    - ompt_work_taskloop
 */
static void
on_ompt_callback_work(
    ompt_work_t              wstype,
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    uint64_t                 count,
    const void              *codeptr_ra)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;
    task_data_t *task_data = (task_data_t*) task->ptr;

    if (wstype == ompt_work_single_executor && endpoint == ompt_scope_begin)
        thread_data->is_single = true;

    LOG_DEBUG_WORK_TYPE(thread_data->id, wstype, count,
        endpoint==ompt_scope_begin?"begin":"end");

    if ((wstype == ompt_work_loop)
      ||(wstype == ompt_work_sections)
      ||(wstype == ompt_work_single_executor)
      ||(wstype == ompt_work_single_other)
      ||(wstype == ompt_work_taskloop))
    {
        if (endpoint == ompt_scope_begin)
        {
            trace_event(thread_data->location, trace_new_workshare_region(
                thread_data->location, wstype, count), endpoint);
        } else {
            trace_event(thread_data->location, NULL, endpoint);
        }
        
    }

    if (wstype == ompt_work_single_executor && endpoint == ompt_scope_end)
        thread_data->is_single = false;

    return;
}

static void
on_ompt_callback_master(
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_target_map(
    ompt_id_t                target_id,
    unsigned int             nitems,
    void *                  *host_addr,
    void *                  *device_addr,
    size_t                  *bytes,
    unsigned int            *mapping_flags,
    const void              *codeptr_ra)
{
    return;
}

/* Sync regions that get standalone graph nodes at sync-end events:
    - ompt_sync_region_barrier
    - ompt_sync_region_barrier_implicit
    - ompt_sync_region_barrier_explicit
    - ompt_sync_region_barrier_implementation
    - ompt_sync_region_taskwait
    - ompt_sync_region_reduction

   Sync regions that get nesting scopes (because they can be nested)
    - ompt_sync_region_taskgroup
 */
static void
on_ompt_callback_sync_region(
    ompt_sync_region_t       kind,
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    const void              *codeptr_ra)
{
    thread_data_t *thread_data = (thread_data_t*) get_thread_data()->ptr;
    task_data_t *task_data = (task_data_t*) task->ptr;

    LOG_DEBUG("[t=%lu] %-6s %s (task=%lu, type=%d)",
        thread_data->id,
        endpoint == ompt_scope_begin ? "begin" : "end",
        kind == ompt_sync_region_barrier                ? "barrier" :
        kind == ompt_sync_region_barrier_implicit       ? "barrier_implicit" :
        kind == ompt_sync_region_barrier_explicit       ? "barrier_explicit" :
        kind == ompt_sync_region_barrier_implementation ? "barrier_implementation" :
        kind == ompt_sync_region_taskwait               ? "taskwait" :
        kind == ompt_sync_region_taskgroup              ? "taskgroup" :
        kind == ompt_sync_region_reduction              ? "reduction" : "unknown",
        task_data->id,
        task_data->type
    );

    if (endpoint == ompt_scope_begin)
    {
        trace_event(thread_data->location, trace_new_sync_region(
            thread_data->location, kind, task_data->id), endpoint);
    } else {
        trace_event(thread_data->location, NULL, endpoint);
    }

    // if (kind == ompt_sync_region_taskgroup)
    // {
    //     if (endpoint == ompt_scope_begin)
    //     {

    //     } else {
            
    //     }
    //     return; // separate return paths by design
    // }    
    return;
}

static void
on_ompt_callback_lock_init(
    ompt_mutex_t             kind,
    unsigned int             hint,
    unsigned int             impl,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_lock_destroy(
    ompt_mutex_t             kind,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_mutex_acquire(
    ompt_mutex_t             kind,
    unsigned int             hint,
    unsigned int             impl,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_mutex_acquired(
    ompt_mutex_t             kind,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_nest_lock(
    ompt_scope_endpoint_t    endpoint,
    ompt_wait_id_t           wait_id,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_flush(
    ompt_data_t             *thread,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_cancel(
    ompt_data_t             *task,
    int                      flags,
    const void              *codeptr_ra)
{
    return;
}

static void
on_ompt_callback_reduction(
    ompt_sync_region_t       kind,
    ompt_scope_endpoint_t    endpoint,
    ompt_data_t             *parallel,
    ompt_data_t             *task,
    const void              *codeptr_ra)
{
    return;
}

unique_id_t
get_unique_id(unique_id_type_t id_type)
{
    /* start counting tasks from 1 so that the initial task is always #1, and
       the root node of the task graph will have ID 0 not attached to any real
       task

       count parallel regions from 1 so the implicit parallel region around the
       whole program is always 0
     */
    static unique_id_t id[NUM_ID_TYPES] = {0,1,0,1};
    return __sync_fetch_and_add(&id[id_type], 1L);
}
