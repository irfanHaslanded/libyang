/**
 * @file tree_data_serde.h
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LY_TREE_DATA_SERDE_H_
#define LY_TREE_DATA_SERDE_H_

#include "in.h"
#include "tree_data.h"

#ifdef __cplusplus
extern "C" {
#endif

LY_ERR lyd_print_flattened(struct lyd_node *node, struct ly_out *out);
LY_ERR lyd_parse_flattened(const struct ly_ctx *ctx, struct ly_in *in, struct lyd_node **root);

#ifdef __cplusplus
}
#endif

#endif /* LY_TREE_DATA_SERDE_H_ */
