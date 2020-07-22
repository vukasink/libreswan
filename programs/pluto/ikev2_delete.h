/* IKEv2 DELETE Exchange
 *
 * Copyright (C) 2020 Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef IKEv2_DELETE_H
#define IKEv2_DELETE_H

#include <stdbool.h>

struct state;
struct ike_sa;
struct child_sa;

bool record_v2_delete(struct ike_sa *ike, struct state *st);
void initiate_v2_delete(struct ike_sa *ike, struct state *st);

#endif
