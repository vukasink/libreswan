/* Connection Database, for libreswan
 *
 * Copyright (C) 2020 Andrew Cagney <cagney@gnu.org>
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

#ifndef CONNECTION_DB_H
#define CONNECTION_DB_H

#include "where.h"
#include "connections.h"

void init_connection_db(void);

struct connection *alloc_connection(const char *name, where_t where);
struct connection *clone_connection(const char *name, struct connection *template, where_t where);

struct spd_route *clone_spd_route(struct connection *c, where_t where);

void add_spd_route_to_db(struct spd_route *sr);
void rehash_spd_route(struct spd_route *sr);
void del_spd_route_from_db(struct spd_route *sr, bool valid);

void add_connection_to_db(struct connection *c, where_t where);
void rehash_connection_that_id(struct connection *c);
void del_connection_from_db(struct connection *c, bool valid);

void check_connection_db(struct logger *logger);
void check_connection(struct connection *connection, struct logger *logger);

#endif
