/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_ITEM_H
#define HAX_SESSION_ITEM_H

#include <string>
#include <string_view>

#include "provider.h"

/* The session item adapter owns the wire representation. Callers receive only the canonical item
 * model and never see Glaze's DTOs or borrowed parser storage. */
/* Encode a borrowed item into `out`, which receives an owned JSON document on success. Returns zero
 * on success or -1 when the input or a wire value cannot be encoded. */
int session_item_encode(const struct item *item, std::string *out);
/* Decode borrowed JSON while it remains valid for the call. Zeroes `out`, then fills it with owned
 * fields released by item_free; returns zero on success or -1 for malformed/incompatible input. */
int session_item_decode(std::string_view input, struct item *out);

#endif /* HAX_SESSION_ITEM_H */
