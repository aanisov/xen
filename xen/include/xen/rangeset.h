/******************************************************************************
 * rangeset.h
 * 
 * Creation, maintenance and automatic destruction of per-domain sets of
 * numeric ranges.
 * 
 * Copyright (c) 2005, K A Fraser
 */

#ifndef __XEN_RANGESET_H__
#define __XEN_RANGESET_H__

#include <xen/types.h>

struct list_head;
struct spinlock;
struct rangeset;

/*
 * Destroy a list of rangesets.
 */
void rangeset_list_destroy(struct list_head *list);

/*
 * Create a rangeset. Optionally attach to a specified list @head.
 * A name may be specified, for use in debug pretty-printing, and various
 * RANGESETF flags (defined below).
 */
struct rangeset *rangeset_new(char *name, unsigned int flags,
                              struct list_head **head);

/*
 * Destroy a rangeset. Rangeset will take an action to remove itself from a
 * list. If a spinlock is provided it will be held during list deletion
 * operation.
 * It is invalid to perform any operation on a rangeset @r
 * after calling rangeset_destroy(r).
 */
void rangeset_destroy(struct rangeset *r, struct spinlock *lock);

/*
 * Set a limit on the number of ranges that may exist in set @r.
 * NOTE: This must be called while @r is empty.
 */
void rangeset_limit(
    struct rangeset *r, unsigned int limit);

/* Flags for passing to rangeset_new(). */
 /* Pretty-print range limits in hexadecimal. */
#define _RANGESETF_prettyprint_hex 0
#define RANGESETF_prettyprint_hex  (1U << _RANGESETF_prettyprint_hex)

bool_t __must_check rangeset_is_empty(
    const struct rangeset *r);

/* Add/remove/query a numeric range. */
int __must_check rangeset_add_range(
    struct rangeset *r, unsigned long s, unsigned long e);
int __must_check rangeset_remove_range(
    struct rangeset *r, unsigned long s, unsigned long e);
bool_t __must_check rangeset_contains_range(
    struct rangeset *r, unsigned long s, unsigned long e);
bool_t __must_check rangeset_overlaps_range(
    struct rangeset *r, unsigned long s, unsigned long e);
int rangeset_report_ranges(
    struct rangeset *r, unsigned long s, unsigned long e,
    int (*cb)(unsigned long s, unsigned long e, void *), void *ctxt);

/* Add/remove/query a single number. */
int __must_check rangeset_add_singleton(
    struct rangeset *r, unsigned long s);
int __must_check rangeset_remove_singleton(
    struct rangeset *r, unsigned long s);
bool_t __must_check rangeset_contains_singleton(
    struct rangeset *r, unsigned long s);

/* swap contents */
void rangeset_swap(struct rangeset *a, struct rangeset *b);

/* Rangeset pretty printing. */
void rangeset_printk(
    struct rangeset *r);
void rangeset_list_printk(
    struct list_head *list);

#endif /* __XEN_RANGESET_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
