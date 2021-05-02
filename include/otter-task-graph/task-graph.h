#if !defined(OTTER_TASK_GRAPH_H)
#define OTTER_TASK_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

#include <macros/debug.h>
#include <otter-common.h>
#include <otter-datatypes/graph.h>

#define TASK_GRAPH_BUFFSZ                   512
#define TASK_GRAPH_DEFAULT_GRAPH_NAME       "OTTER_TASK_GRAPH"
#define TASK_GRAPH_DEFAULT_GRAPH_ATTR_NAME  "OTTER_TASK_GRAPH_NODE_ATTR.csv"
#define PID_STR_SZ 32

/* Alias graph types to distinguish graph elements created/owned by task graph
   from those used to build temporary sub-graphs which are eventually added to
   the task graph.
*/
typedef graph_node_t         task_graph_node_t;
typedef graph_node_data_t    task_graph_node_data_t;

/* Represents the types of nodes the task graph can contain. Closely aligned to
   the scope_t enum in otter.h */
#define FLAG_NODE_TYPE_END(f) (f & 0x1000)
typedef enum {

    node_type_unknown,

    /* OMP task types */
    node_task_initial,
    node_task_implicit,
    node_task_explicit,
    node_task_target,

    /* Scope begin nodes */
    node_scope_parallel_begin,
    node_scope_sections_begin,
    node_scope_single_begin,
    node_scope_loop_begin,
    node_scope_taskloop_begin,
    node_scope_sync_taskgroup_begin,

    /* Matching endpoints - switch on a flag for these so that a node's metadata
       is only freed when the context-end node is popped from the graph's node
       stack */
    node_scope_parallel_end = FLAG_NODE_TYPE_END(node_scope_parallel_begin),
    node_scope_sections_end,
    node_scope_single_end,
    node_scope_loop_end,
    node_scope_taskloop_end,
    node_scope_sync_taskgroup_end,

    /* Standalone (i.e. never nested) synchronisation directives
       (not scopes)
       restart numbering from before flag
    */
    node_sync_barrier = node_scope_sync_taskgroup_begin + 1,
    node_sync_barrier_implicit,
    node_sync_barrier_explicit,
    node_sync_barrier_implementation,
    node_sync_taskwait,
    node_sync_taskgroup,
    node_sync_reduction

    /* etc... */
} task_graph_node_type_t;

// task graph functions
bool task_graph_init(otter_opt_t *opt);
bool task_graph_write(void);
void task_graph_destroy(graph_free_node_data_t free_node_data);

/* add a node to the graph and return a reference to it */
task_graph_node_t *task_graph_add_node(
    task_graph_node_type_t node_type, task_graph_node_data_t node_data);

/* declare an edge from src to dest */
void task_graph_add_edge(task_graph_node_t *src, task_graph_node_t *dest);

/* move the nodes and edges from the subgraph into the task graph. does not
   create any edges between the two graphs. after this operation subgraph will
   be empty
*/
bool task_graph_attach_subgraph(graph_t *subgraph);

/* TODO:
    - write node attributes to json (differentiate fields by node type)
 */

#endif // OTTER_TASK_GRAPH_H
