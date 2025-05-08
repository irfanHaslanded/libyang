#define _GNU_SOURCE

#include "printer_data.h"
#include "tree_data.h"
#include "tree_data_serde.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "context.h"
#include "dict.h"
#include "diff.h"
#include "hash_table.h"
#include "in.h"
#include "in_internal.h"
#include "log.h"
#include "ly_common.h"
#include "out_internal.h"
#include "parser_data.h"
#include "parser_internal.h"
#include "path.h"
#include "plugins.h"
#include "plugins_exts/metadata.h"
#include "plugins_internal.h"
#include "plugins_types.h"
#include "set.h"
#include "tree.h"
#include "tree_data_internal.h"
#include "tree_data_sorted.h"
#include "tree_edit.h"
#include "tree_schema.h"
#include "tree_schema_internal.h"
#include "validation.h"
#include "xml.h"
#include "xpath.h"

/* Format to use to serialize opaque nodes */
#define LYD_FLAT_OPAQ_FORMAT LYD_LYB

/* need array to lookup schema_id to schema ptr */
/* need to store data so we can call lyd_create_term() like functions directly */
typedef uint32_t lyd_flat_id_t;

struct lyd_flat_value {
    lyd_flat_id_t id;
    uint8_t type;
    uint32_t hash;
    uint32_t value_len;
    char *value;
};

struct lyd_flat_value_printer {
    lyd_flat_id_t id;
    uint8_t type;
    uint32_t hash;
    uint32_t value_len;
    const char *value;

    /* this is not serialized */
    struct lyd_flat_value_printer *next;
    int free; /* should this value be free'd */
};

struct lyd_flat_node {
    lyd_flat_id_t id;
    lyd_flat_id_t schema_id;
    lyd_flat_id_t parent_id;
    lyd_flat_id_t child_id;
    lyd_flat_id_t meta_id;
    lyd_flat_id_t next_id;
    lyd_flat_id_t value_id;

    uint32_t hash;
    uint32_t flags;
};

struct lyd_flat_node_printer {
    lyd_flat_id_t id;
    lyd_flat_id_t schema_id;
    lyd_flat_id_t parent_id;
    lyd_flat_id_t child_id;
    lyd_flat_id_t meta_id;
    lyd_flat_id_t next_id;
    lyd_flat_id_t value_id;

    uint32_t hash;
    uint32_t flags;

    /* this is not serialized */
    struct lyd_flat_node_printer *next;
};

struct lyd_flat_meta {
    lyd_flat_id_t id;
    lyd_flat_id_t parent_id;
    lyd_flat_id_t next_id;

    uint32_t mod_name_len;
    uint32_t meta_name_len;
    uint32_t value_len;
    char *mod_name;
    char *meta_name;
    char *value;
};

struct lyd_flat_meta_printer {
    lyd_flat_id_t id;
    lyd_flat_id_t parent_id;
    lyd_flat_id_t next_id;

    uint32_t mod_name_len;
    uint32_t meta_name_len;
    uint32_t value_len;
    const char *mod_name;
    const char *meta_name;
    const char *value;

    /* this is not serialized */
    struct lyd_flat_meta_printer *next;
};

struct lyd_tree_flat_printer {
    struct lyd_flat_node_printer *nodes;
    struct lyd_flat_node_printer *last_node;
    uint32_t node_count;
    struct lyd_flat_meta_printer *meta;
    struct lyd_flat_meta_printer *last_meta;
    uint32_t meta_count;
    struct lyd_flat_value_printer *values;
    struct lyd_flat_value_printer *last_val;
    uint32_t val_count;
};

struct lyd_tree_flat_parser {
    uint32_t node_count;
    uint32_t meta_count;
    uint32_t val_count;
    struct lyd_flat_node *nodes;
    struct lyd_flat_meta *meta;
    struct lyd_flat_value *values;
};

static LY_ERR
lyd_flatten_meta(struct lyd_meta *meta, lyd_flat_id_t parent_id, int siblings, struct lyd_tree_flat_printer *flat_tree)
{
    struct lyd_flat_meta_printer *tmp;

    assert(meta);

    /* skip over any unprintable meta */
    while (!lyd_metadata_should_print(meta)) {
        if (!siblings || !(meta = meta->next)) {
            return LY_SUCCESS;
        }
    }

    tmp = calloc(1, sizeof *tmp);
    tmp->id = ++flat_tree->meta_count;
    tmp->parent_id = parent_id;

    tmp->mod_name = meta->annotation->module->name;
    tmp->mod_name_len = strlen(tmp->mod_name);
    tmp->meta_name = meta->name;
    tmp->meta_name_len = strlen(tmp->meta_name);
    tmp->value = lyd_get_meta_value(meta);
    tmp->value_len = strlen(tmp->value);

    if (flat_tree->last_meta) {
        flat_tree->last_meta->next = tmp;
    } else {
        flat_tree->meta = tmp;
    }
    flat_tree->last_meta = tmp;
    while (siblings && (meta = meta->next)) {
        lyd_flatten_meta(meta, parent_id, 0, flat_tree);
        /* if a new meta was assigned, make it our next */
        if (flat_tree->last_meta != tmp) {
            tmp->next_id = flat_tree->last_meta->id;
            tmp = flat_tree->last_meta;
        }
    }

    return LY_SUCCESS;
}

static LY_ERR
lyd_flatten_any(struct lyd_node_any *any, struct lyd_flat_value_printer **val_printer)
{
    struct ly_out *out = NULL;
    struct lyd_flat_value_printer *val = calloc(1, sizeof *val);
    char *buf = NULL;

    switch (any->value_type) {
    case LYD_ANYDATA_DATATREE:
        if (!any->value.tree) {
            break;
        }
        ly_out_new_memory(&buf, 0, &out);
        lyd_print_all(out, any->value.tree, LYD_FLAT_OPAQ_FORMAT, 0);
        val->value_len = ly_out_printed(out);
        /* NULL termination */
        buf[val->value_len - 1] = '\0';
        val->value = buf;
        val->free = 1;
        ly_out_free(out, NULL, 0);
        break;
    case LYD_ANYDATA_XML:
    case LYD_ANYDATA_STRING:
        if (!any->value.str) {
            break;
        }
        val->value = any->value.str;
        val->value_len = val->value ? strlen(any->value.str) : 0;
        break;
    case LYD_ANYDATA_JSON:
        if (!any->value.json) {
            break;
        }
        val->value = any->value.json;
        val->value_len = val->value ? strlen(any->value.json) : 0;
        break;
    case LYD_ANYDATA_LYB:
        if (!any->value.mem) {
            break;
        }
        val->value = any->value.mem;
        val->value_len = val->value ? lyd_lyb_data_length(any->value.mem) : 0;
        break;
    default:
        assert(0);
        break;
    }

    val->type = (any->value_type == LYD_ANYDATA_DATATREE) ? LYD_ANYDATA_LYB : any->value_type;
    *val_printer = val;

    return LY_SUCCESS;
}

static LY_ERR
lyd_flatten_opaq(struct lyd_node *opaq, struct lyd_flat_value_printer **val_printer)
{
    struct ly_out *out = NULL;
    struct lyd_flat_value_printer *val = calloc(1, sizeof *val);
    char *buf = NULL;

    assert(!opaq->schema);

    ly_out_new_memory(&buf, 0, &out);
    // TODO FIXME print siblings ?
    lyd_print_tree(out, opaq, LYD_FLAT_OPAQ_FORMAT, 0);
    val->value_len = ly_out_printed(out);
    /* NULL termination */
    buf[val->value_len - 1] = '\0';
    val->value = buf;
    val->free = 1;
    ly_out_free(out, NULL, 0);

    if (val->value) {
        *val_printer = val;
    } else {
        /* free val if there is no value */
        free(val);
    }

    return LY_SUCCESS;
}

static LY_ERR
lyd_flatten_tree(struct lyd_node *node, lyd_flat_id_t parent_id, int siblings, struct lyd_tree_flat_printer *flat_tree)
{
    int ret = LY_SUCCESS;
    struct lyd_flat_node_printer *tmp = calloc(1, sizeof *tmp);
    struct lyd_node *next = NULL, *child = NULL;
    struct lyd_node_any *any = NULL;
    struct lyd_flat_value_printer *val = NULL;
    const char *value = NULL;

    if (!node) {
        return LY_SUCCESS;
    }

    tmp->hash = node->hash;
    tmp->flags = node->flags;
    tmp->schema_id = node->schema ? node->schema->schema_id : 0;
    tmp->parent_id = parent_id;

    if (flat_tree->last_node) {
        flat_tree->last_node->next = tmp;
    } else {
        flat_tree->nodes = tmp;
    }
    flat_tree->last_node = tmp;
    tmp->id = ++flat_tree->node_count;

    if (node->meta) {
        /* first meta will be assigned next_id */
        tmp->meta_id = flat_tree->meta_count + 1;
        lyd_flatten_meta(node->meta, tmp->id, 1, flat_tree);
    }

    if (node->schema) {
        if ((value = lyd_get_value(node))) {
            val = calloc(1, sizeof *val);
            val->value = value;
            val->value_len = strlen(value);
        } else if (node->schema->nodetype & LYD_NODE_ANY) {
            any = (struct lyd_node_any *) node;
            LY_CHECK_RET(lyd_flatten_any(any, &val));
        }
    } else {
        LY_CHECK_RET(lyd_flatten_opaq(node, &val));
    }

    if (val) {
        val->id = ++flat_tree->val_count;

        if (node->schema && node->schema->nodetype & LYD_NODE_TERM) {
            val->type = ((struct lyd_node_term *)node)->value.realtype->basetype;
            val->hash = lyht_hash(value, val->value_len);
        }
        assert(!val->next);
        if (flat_tree->last_val) {
            flat_tree->last_val->next = val;
        } else {
            flat_tree->values = val;
        }
        flat_tree->last_val = val;
        tmp->value_id = val->id;
    }

    if (node->schema && (node->schema->nodetype & LYD_NODE_INNER) && (child = ((struct lyd_node_inner *)node)->child)) {
        /* first child will be assigned next_id */
        tmp->child_id = flat_tree->node_count + 1;
        lyd_flatten_tree(child, tmp->id, 1, flat_tree);
    }

    /* if there is a next sibling, it will get the next id */
    if (node->next) {
        tmp->next_id = flat_tree->node_count + 1;
    }

    if (siblings) {
        for (next = node->next; next; next = next->next) {
            lyd_flatten_tree(next, parent_id, 0, flat_tree);
        }
    }

    return ret;
}

LY_ERR
flat_print_data(struct ly_out *out, const struct lyd_node *node, uint32_t options)
{
    int ret = LY_SUCCESS;
    struct lyd_tree_flat_printer flat_tree = {0};
    struct lyd_flat_node_printer *tmp, *tmp_next;
    struct lyd_flat_meta_printer *meta, *meta_next;
    struct lyd_flat_value_printer *value, *value_next;

    LY_CHECK_RET(lyd_flatten_tree(node, 0, options & LYD_PRINT_WITHSIBLINGS, &flat_tree));

    LY_CHECK_GOTO(ly_write_(out, (const char *)&flat_tree.node_count, sizeof flat_tree.node_count), cleanup);
    LY_CHECK_GOTO(ly_write_(out, (const char *)&flat_tree.meta_count, sizeof flat_tree.meta_count), cleanup);
    LY_CHECK_GOTO(ly_write_(out, (const char *)&flat_tree.val_count, sizeof flat_tree.val_count), cleanup);

    for (tmp = flat_tree.nodes; tmp; tmp = tmp->next) {
        LY_CHECK_GOTO(ly_write_(out, (const char *)tmp, sizeof (struct lyd_flat_node)), cleanup);
    }

    for (meta = flat_tree.meta; meta; meta = meta->next) {
        LY_CHECK_GOTO(ly_write_(out, (const char *)meta, sizeof (struct lyd_flat_meta) - 3 * sizeof(char *)), cleanup);
        LY_CHECK_GOTO(ly_write_(out, meta->mod_name, meta->mod_name_len), cleanup);
        LY_CHECK_GOTO(ly_write_(out, meta->meta_name, meta->meta_name_len), cleanup);
        LY_CHECK_GOTO(ly_write_(out, meta->value, meta->value_len), cleanup);
    }

    for (value = flat_tree.values; value; value = value->next) {
        LY_CHECK_GOTO(ly_write_(out, (const char *)value, sizeof (struct lyd_flat_value) - sizeof (char *)), cleanup);
        LY_CHECK_GOTO(ly_write_(out, value->value, value->value_len), cleanup);
    }
    ly_print_flush(out);

cleanup:
    for (tmp = flat_tree.nodes; tmp; tmp = tmp_next) {
        tmp_next = tmp->next;
        free(tmp);
    }
    for (meta = flat_tree.meta; meta; meta = meta_next) {
        meta_next = meta->next;
        free(meta);
    }
    for (value = flat_tree.values; value; value = value_next) {
        value_next = value->next;
        if (value->free) {
            /* although this is a const char *, this was dynamically allocated */
            free((void *)value->value);
        }
        free(value);
    }

    return ret;
}

static LY_ERR
lyd_parse_flat_single(const struct ly_ctx *ctx, struct lyd_flat_node *flat_node, struct lyd_flat_value *value, struct lyd_node **out)
{
    const struct lysc_node *schema = NULL;
    struct lyd_node_term *term = NULL;
    int ret = LY_SUCCESS;
    LY_VALUE_FORMAT val_format = LY_VALUE_CANON;

    if (!flat_node) {
        return LY_SUCCESS;
    }

    if (!flat_node->schema_id) {
        ret = lyd_parse_data_mem(ctx, value->value, LYD_FLAT_OPAQ_FORMAT, LYD_PARSE_OPAQ | LYD_PARSE_STORE_ONLY, 0, out);
        assert(!ret);
        return LY_SUCCESS;
    }

    schema = ly_ctx_get_schema_from_id(ctx, flat_node->schema_id);

    if (schema->nodetype & LYD_NODE_TERM) {
        term = calloc(1, sizeof *term);
        LY_CHECK_ERR_RET(!term, LOGMEM(schema->module->ctx), LY_EMEM);

        term->schema = schema;
        term->prev = &term->node;

        if (value) {
            if (value->type == LY_TYPE_INST) {
                val_format = LY_VALUE_JSON;
            }
            ret = lyd_value_store(ctx, &term->value, ((struct lysc_node_leaf *)schema)->type, value->value, value->value_len, 1, 1, NULL, val_format, NULL,
                    LYD_HINT_DATA, schema, NULL);
        }
        *out = (struct lyd_node *)term;
    }

    if (schema->nodetype & LYD_NODE_INNER) {
        ret = lyd_create_inner(schema, out);
    }

    if (schema->nodetype & LYD_NODE_ANY) {
        if (value->value) {
            ret = lyd_create_any(schema, value->value, value->type, 0, out);
        } else {
            ret = lyd_create_any(schema, NULL, LYD_ANYDATA_DATATREE, 1, out);
        }
    }

    if (*out) {
        (*out)->flags = flat_node->flags;
        (*out)->hash = flat_node->hash;
    }

    return ret;
}

static LY_ERR
lyd_parse_flat_tree(const struct ly_ctx *ctx, struct lyd_tree_flat_parser *flat_tree, struct lyd_node **root)
{
    struct lyd_flat_node *tmp;
    int ret = LY_SUCCESS;
    struct lyd_node **nodes = NULL;
    struct lyd_node *prev;
    struct lyd_flat_meta *flat_meta;
    struct lys_module *mod;
    uint32_t i;

    /* NULL for safety */
    *root = NULL;

    nodes = calloc(flat_tree->node_count, sizeof *nodes);
    // TODO memcheck

    /* run a first-pass of parse to construct all nodes */
    for (i = 0; i < flat_tree->node_count; i++) {
        tmp = &flat_tree->nodes[i];
        if ((ret = lyd_parse_flat_single(ctx, tmp, tmp->value_id ? &flat_tree->values[tmp->value_id - 1] : NULL, &nodes[i]))) {
            assert(0);
        }
    }

    for (i = 0; i < flat_tree->meta_count; i++) {
        flat_meta = &flat_tree->meta[i];
        mod = ly_ctx_get_module_implemented2(ctx, flat_meta->mod_name, flat_meta->mod_name_len);
        LY_CHECK_GOTO(ret = lyd_create_meta(nodes[flat_meta->parent_id - 1], NULL, mod,
                flat_meta->meta_name, flat_meta->meta_name_len,
                flat_meta->value, flat_meta->value_len, 1, 1, NULL, LY_VALUE_JSON,
                NULL, LYD_HINT_DATA, nodes[flat_meta->parent_id - 1]->schema, 0, NULL), cleanup);
    }

    /* run a second-pass to create all the relationship links */
    for (i = 0; i < flat_tree->node_count; i++) {
        tmp = &flat_tree->nodes[i];
        /* link with pareent */
        if (tmp->parent_id) {
            assert(nodes[i]);
            nodes[i]->parent = (struct lyd_node_inner *)nodes[tmp->parent_id - 1];
            assert(nodes[i]->parent);
            /* paranoid check */
            assert(nodes[i]->parent->schema->nodetype & LYD_NODE_INNER);
        }
        /* link with child */
        if (tmp->child_id) {
            assert(nodes[tmp->child_id - 1]);
            ((struct lyd_node_inner *)(nodes[i]))->child = nodes[tmp->child_id - 1];
        }
        /* link with next sibling and fixup prev as well */
        if (tmp->next_id) {
            nodes[i]->next = nodes[tmp->next_id - 1];
            assert(nodes[i]->next);
            nodes[i]->next->prev = nodes[i];
        } else {
            if (nodes[i]->parent) {
                /* we have a parent, so, first sibling is parent->child */
                prev = nodes[i]->parent->child;
            } else {
                /* walk back to the first sibling */
                for (prev = nodes[i]; prev != prev->prev; prev = prev->prev) {}
            }
            /* last node must be first node's previous */
            prev->prev = nodes[i];
        }
    }

    /* run a third pass to construct children_ht */
    for (i = 0; i < flat_tree->node_count; i++) {
        tmp = &flat_tree->nodes[i];
        if (tmp->child_id) {
            // TODO FIXME insert hash how?
            lyd_insert_hash(nodes[tmp->child_id - 1]);
        }
    }

    /* first node is always the root */
    *root = nodes[0];

cleanup:
    free(nodes);

    return ret;
}

LY_ERR
lyd_parse_flattened(const struct ly_ctx *ctx, struct ly_in *in, struct lyd_node **root)
{
    struct lyd_tree_flat_parser *flat_tree = calloc(1, sizeof *flat_tree);
    uint32_t i;

    /* read in node_count, meta_count and val_count */
    LY_CHECK_RET(ly_in_read(in, flat_tree, 3 * sizeof(uint32_t)));

    /* TODO skip redundant calloc and copy, and just directly point to the memory? */
    /* can only be done when in is of type memory */
    flat_tree->nodes = calloc(flat_tree->node_count, sizeof(*flat_tree->nodes));
    flat_tree->meta = calloc(flat_tree->meta_count, sizeof(*flat_tree->meta));
    flat_tree->values = calloc(flat_tree->val_count, sizeof(*flat_tree->values));

    LY_CHECK_RET(ly_in_read(in, flat_tree->nodes, flat_tree->node_count * sizeof(*flat_tree->nodes)));

    for (i = 0; i < flat_tree->meta_count; i++) {
        LY_CHECK_RET(ly_in_read(in, &flat_tree->meta[i], sizeof (struct lyd_flat_meta) - 3 * sizeof(char *)));
        flat_tree->meta[i].mod_name = calloc(1, 1 + flat_tree->meta[i].mod_name_len);
        LY_CHECK_RET(ly_in_read(in, flat_tree->meta[i].mod_name, flat_tree->meta[i].mod_name_len));
        flat_tree->meta[i].meta_name = calloc(1, 1 + flat_tree->meta[i].meta_name_len);
        LY_CHECK_RET(ly_in_read(in, flat_tree->meta[i].meta_name, flat_tree->meta[i].meta_name_len));
        flat_tree->meta[i].value = calloc(1, 1 + flat_tree->meta[i].value_len);
        LY_CHECK_RET(ly_in_read(in, flat_tree->meta[i].value, flat_tree->meta[i].value_len));
    }

    for (i = 0; i < flat_tree->val_count; i++) {
        LY_CHECK_RET(ly_in_read(in, &flat_tree->values[i], sizeof(*flat_tree->values) - sizeof(char *)));
        if (flat_tree->values[i].value_len) {
            flat_tree->values[i].value = calloc(1, 1 + flat_tree->values[i].value_len);
            LY_CHECK_RET(ly_in_read(in, flat_tree->values[i].value, flat_tree->values[i].value_len));
        }
    }

    /* construct */
    lyd_parse_flat_tree(ctx, flat_tree, root);

    /* free memory */
    for (i = 0; i < flat_tree->meta_count; i++) {
        free(flat_tree->meta[i].mod_name);
        free(flat_tree->meta[i].meta_name);
        free(flat_tree->meta[i].value);
    }

    for (i = 0; i < flat_tree->val_count; i++) {
        free(flat_tree->values[i].value);
    }
    free(flat_tree->nodes);
    free(flat_tree->meta);
    free(flat_tree->values);
    free(flat_tree);

    return LY_SUCCESS;
}

// TODO FIXME
//
// somehow know the ly ctx matches the printer and parser
