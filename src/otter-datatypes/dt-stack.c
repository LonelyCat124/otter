#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <macros/debug.h>
#include <otter-datatypes/stack.h>

typedef struct node_t node_t;

struct node_t {
    data_item_t    data;
    node_t         *next;
};

struct stack_t {
    node_t      *head;
    size_t       size;
};

stack_t *
stack_create(void)
{
    stack_t *s = malloc(sizeof(*s));
    if (s == NULL)
    {
        LOG_ERROR("failed to create stack");
        return NULL;
    }
    LOG_DEBUG("%p", s);
    s->head = NULL;
    s->size = 0;
    return s;
}

bool           
stack_push(stack_t *s, data_item_t item)
{
    if (s == NULL)
    {
        LOG_WARN("stack is null, can't add item");
        return false;
    }

    node_t *node = malloc(sizeof(*node));

    if (node == NULL)
    {
        LOG_ERROR("failed to push item onto stack %p", s);
        return false;
    }

    node->data = item;
    node->next = s->head;
    s->head = node;
    s->size += 1;

    LOG_DEBUG("%p[0]=%p", s, item.ptr);

    return true;
}

bool   
stack_pop(stack_t *s, data_item_t *dest)
{
    if (s == NULL)
    {
        LOG_WARN("stack is null");
        return false;
    }

    if (s->head == NULL)
    {
        LOG_DEBUG("%p[0]=%p", s, s->head);
        return false;
    }

    node_t *node = s->head;
    if (node != NULL)
    {
        if (dest != NULL) *dest = node->data;
        s->head = s->head->next;
        s->size -= 1;
        free(node);
    }
    LOG_DEBUG_IF((dest != NULL), "%p[0] -> %p", s, dest->ptr);
    LOG_WARN_IF(dest == NULL, "popped item without returning value "
                              "(no destination pointer)");

    return true;
}

bool
stack_peek(stack_t *s, data_item_t *dest)
{
    if ((s == NULL) || (dest == NULL) || ((s->head == NULL)))
        return false;
    *dest = s->head->data;
    return true;
}

size_t         
stack_size(stack_t *s)
{
    return (s == NULL) ? 0 : s->size;
}

bool           
stack_is_empty(stack_t *s)
{
    return (s == NULL) ? true : ((s->size == 0) ? true : false) ;
}

void           
stack_destroy(stack_t *s, bool items, data_destructor_t destructor)
{
    if (s == NULL) return;
    LOG_WARN_IF((s->size != 0 && items == false),
        "destroying stack %p (len=%lu) without destroying items may cause "
        "memory leak", s, s->size);
    data_item_t d = {.ptr = NULL};
    while(stack_pop(s, &d))
    {
        LOG_DEBUG("%p[0/%lu]=%p", s, s->size-1, d.ptr);
        if (items) destructor != NULL ? destructor(d.ptr) : free(d.ptr) ;
    }
    LOG_DEBUG("%p", s);
    free(s);
    return;
}

#if DEBUG_LEVEL >= 4
void
stack_print(stack_t *s)
{
    if (s == NULL)
    {
        fprintf(stderr, "\n%12s\n", "<null stack>");
        return;
    }

    node_t *node = s->head;

    fprintf(stderr, "\n"
                    "%12s %p\n"
                    "%12s %p\n"
                    "%12s %lu\n",
                    "stack",        s,
                    "head node",    s->head,
                    "size",         s->size);

    const char *sep = " | ";
    fprintf(stderr, "%12s%s%-12s%s%-8s\n", "position", sep, "node", sep, "item");
    int position = 0;
    while (node != NULL)
    {
        fprintf(stderr, "%12d%s%-12p%s0x%06lx (%lu)\n",
            position, sep, node, sep, node->data.value, node->data.value);
        node = node->next;
        position++;
    }
    fprintf(stderr, "\n");
    return;
}
#endif
