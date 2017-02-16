/******************************************************************************
 * rangeset.c
 * 
 * Creation, maintenance and automatic destruction of per-domain sets of
 * numeric ranges.
 * 
 * Copyright (c) 2005, K A Fraser
 */

#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/rangeset.h>
#include <xsm/xsm.h>

/* An inclusive range [s,e] and pointer to next range in ascending order. */
struct range {
    struct list_head list;
    unsigned long s, e;
};

struct rangeset {
    /* threaded list of rangesets. */
    struct list_head rangeset_list;

    /* Ordered list of ranges contained in this set, and protecting lock. */
    struct list_head range_list;

    /* Number of ranges that can be allocated */
    long             nr_ranges;
    rwlock_t         lock;

    /* Pretty-printing name. */
    char             name[32];

    /* RANGESETF flags. */
    unsigned int     flags;
};

/*****************************
 * Private range functions hide the underlying linked-list implemnetation.
 */

/* Find highest range lower than or containing s. NULL if no such range. */
static struct range *find_range(
    struct rangeset *r, unsigned long s)
{
    struct range *x = NULL, *y;

    list_for_each_entry ( y, &r->range_list, list )
    {
        if ( y->s > s )
            break;
        x = y;
    }

    return x;
}

/* Return the lowest range in the set r, or NULL if r is empty. */
static struct range *first_range(
    struct rangeset *r)
{
    if ( list_empty(&r->range_list) )
        return NULL;
    return list_entry(r->range_list.next, struct range, list);
}

/* Return range following x in ascending order, or NULL if x is the highest. */
static struct range *next_range(
    struct rangeset *r, struct range *x)
{
    if ( x->list.next == &r->range_list )
        return NULL;
    return list_entry(x->list.next, struct range, list);
}

/* Insert range y after range x in r. Insert as first range if x is NULL. */
static void insert_range(
    struct rangeset *r, struct range *x, struct range *y)
{
    list_add(&y->list, (x != NULL) ? &x->list : &r->range_list);
}

/* Remove a range from its list and free it. */
static void destroy_range(
    struct rangeset *r, struct range *x)
{
    r->nr_ranges++;

    list_del(&x->list);
    xfree(x);
}

/* Allocate a new range */
static struct range *alloc_range(
    struct rangeset *r)
{
    struct range *x;

    if ( r->nr_ranges == 0 )
        return NULL;

    x = xmalloc(struct range);
    if ( x )
        --r->nr_ranges;

    return x;
}

/*****************************
 * Core public functions
 */

int rangeset_add_range(
    struct rangeset *r, unsigned long s, unsigned long e)
{
    struct range *x, *y;
    int rc = 0;

    ASSERT(s <= e);

    write_lock(&r->lock);

    x = find_range(r, s);
    y = find_range(r, e);

    if ( x == y )
    {
        if ( (x == NULL) || ((x->e < s) && ((x->e + 1) != s)) )
        {
            x = alloc_range(r);
            if ( x == NULL )
            {
                rc = -ENOMEM;
                goto out;
            }

            x->s = s;
            x->e = e;

            insert_range(r, y, x);
        }
        else if ( x->e < e )
            x->e = e;
    }
    else
    {
        if ( x == NULL )
        {
            x = first_range(r);
            x->s = s;
        }
        else if ( (x->e < s) && ((x->e + 1) != s) )
        {
            x = next_range(r, x);
            x->s = s;
        }
        
        x->e = (y->e > e) ? y->e : e;

        for ( ; ; )
        {
            y = next_range(r, x);
            if ( (y == NULL) || (y->e > x->e) )
                break;
            destroy_range(r, y);
        }
    }

    y = next_range(r, x);
    if ( (y != NULL) && ((x->e + 1) == y->s) )
    {
        x->e = y->e;
        destroy_range(r, y);
    }

 out:
    write_unlock(&r->lock);
    return rc;
}

int rangeset_remove_range(
    struct rangeset *r, unsigned long s, unsigned long e)
{
    struct range *x, *y, *t;
    int rc = 0;

    ASSERT(s <= e);

    write_lock(&r->lock);

    x = find_range(r, s);
    y = find_range(r, e);

    if ( x == y )
    {
        if ( (x == NULL) || (x->e < s) )
            goto out;

        if ( (x->s < s) && (x->e > e) )
        {
            y = alloc_range(r);
            if ( y == NULL )
            {
                rc = -ENOMEM;
                goto out;
            }

            y->s = e + 1;
            y->e = x->e;
            x->e = s - 1;

            insert_range(r, x, y);
        }
        else if ( (x->s == s) && (x->e <= e) )
            destroy_range(r, x);
        else if ( x->s == s )
            x->s = e + 1;
        else if ( x->e <= e )
            x->e = s - 1;
    }
    else
    {
        if ( x == NULL )
            x = first_range(r);

        if ( x->s < s )
        {
            x->e = s - 1;
            x = next_range(r, x);
        }

        while ( x != y )
        {
            t = x;
            x = next_range(r, x);
            destroy_range(r, t);
        }

        x->s = e + 1;
        if ( x->s > x->e )
            destroy_range(r, x);
    }

 out:
    write_unlock(&r->lock);
    return rc;
}

bool_t rangeset_contains_range(
    struct rangeset *r, unsigned long s, unsigned long e)
{
    struct range *x;
    bool_t contains;

    ASSERT(s <= e);

    read_lock(&r->lock);
    x = find_range(r, s);
    contains = (x && (x->e >= e));
    read_unlock(&r->lock);

    return contains;
}

bool_t rangeset_overlaps_range(
    struct rangeset *r, unsigned long s, unsigned long e)
{
    struct range *x;
    bool_t overlaps;

    ASSERT(s <= e);

    read_lock(&r->lock);
    x = find_range(r, e);
    overlaps = (x && (s <= x->e));
    read_unlock(&r->lock);

    return overlaps;
}

int rangeset_report_ranges(
    struct rangeset *r, unsigned long s, unsigned long e,
    int (*cb)(unsigned long s, unsigned long e, void *), void *ctxt)
{
    struct range *x;
    int rc = 0;

    read_lock(&r->lock);

    for ( x = first_range(r); x && (x->s <= e) && !rc; x = next_range(r, x) )
        if ( x->e >= s )
            rc = cb(max(x->s, s), min(x->e, e), ctxt);

    read_unlock(&r->lock);

    return rc;
}

int rangeset_add_singleton(
    struct rangeset *r, unsigned long s)
{
    return rangeset_add_range(r, s, s);
}

int rangeset_remove_singleton(
    struct rangeset *r, unsigned long s)
{
    return rangeset_remove_range(r, s, s);
}

bool_t rangeset_contains_singleton(
    struct rangeset *r, unsigned long s)
{
    return rangeset_contains_range(r, s, s);
}

bool_t rangeset_is_empty(
    const struct rangeset *r)
{
    return ((r == NULL) || list_empty(&r->range_list));
}

struct rangeset *rangeset_new(char *name, unsigned int flags,
                              struct list_head **head)
{
    struct rangeset *r;

    r = xmalloc(struct rangeset);
    if ( r == NULL )
        return NULL;

    rwlock_init(&r->lock);
    INIT_LIST_HEAD(&r->range_list);
    r->nr_ranges = -1;

    BUG_ON(flags & ~RANGESETF_prettyprint_hex);
    r->flags = flags;

    if ( name != NULL )
    {
        safe_strcpy(r->name, name);
    }
    else
    {
        snprintf(r->name, sizeof(r->name), "(no name)");
    }

    if (head)
        *head = &r->rangeset_list;

    return r;
}

void rangeset_destroy(
    struct rangeset *r, spinlock_t *lock)
{
    struct range *x;

    if ( r == NULL )
        return;

    if ( lock )
        spin_lock(lock);

    list_del(&r->rangeset_list);

    if ( lock )
        spin_unlock(lock);

    while ( (x = first_range(r)) != NULL )
        destroy_range(r, x);

    xfree(r);
}

void rangeset_limit(
    struct rangeset *r, unsigned int limit)
{
    r->nr_ranges = limit;
}

void rangeset_list_destroy(struct list_head *list)
{
    struct rangeset *r;

    while ( !list_empty(list) )
    {
        r = list_entry(list->next, struct rangeset, rangeset_list);

        list_del(&r->rangeset_list);

        rangeset_destroy(r, NULL);
    }
}

void rangeset_swap(struct rangeset *a, struct rangeset *b)
{
    LIST_HEAD(tmp);

    if ( a < b )
    {
        write_lock(&a->lock);
        write_lock(&b->lock);
    }
    else
    {
        write_lock(&b->lock);
        write_lock(&a->lock);
    }

    list_splice_init(&a->range_list, &tmp);
    list_splice_init(&b->range_list, &a->range_list);
    list_splice(&tmp, &b->range_list);

    write_unlock(&a->lock);
    write_unlock(&b->lock);
}

/*****************************
 * Pretty-printing functions
 */

static void print_limit(struct rangeset *r, unsigned long s)
{
    printk((r->flags & RANGESETF_prettyprint_hex) ? "%lx" : "%lu", s);
}

void rangeset_printk(
    struct rangeset *r)
{
    int nr_printed = 0;
    struct range *x;

    read_lock(&r->lock);

    printk("%-10s {", r->name);

    for ( x = first_range(r); x != NULL; x = next_range(r, x) )
    {
        if ( nr_printed++ )
            printk(",");
        printk(" ");
        print_limit(r, x->s);
        if ( x->s != x->e )
        {
            printk("-");
            print_limit(r, x->e);
        }
    }

    printk(" }");

    read_unlock(&r->lock);
}

void rangeset_list_printk(
    struct list_head *list)
{
    struct rangeset *r;

    list_for_each_entry ( r, list, rangeset_list )
    {
        printk("    ");
        rangeset_printk(r);
        printk("\n");
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
