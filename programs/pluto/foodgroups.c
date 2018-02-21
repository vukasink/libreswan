/* Implement policy groups-style control files (aka "foodgroups")
 * Copyright (C) 2002  D. Hugh Redelmeier.
 * Copyright (C) 2005 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009-2010 Paul Wouters <paul@xelerance.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h> /* PATH_MAX */

#include <libreswan.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "id.h"
#include "x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "foodgroups.h"
#include "kernel.h"             /* needs connections.h */
#include "lswconf.h"
#include "lex.h"
#include "log.h"
#include "whack.h"

#include <errno.h>

/* Food group config files are found in directory fg_path */

static char *fg_path = NULL;
static size_t fg_path_space = 0;

/* Groups is a list of connections that are policy groups.
 * The list is updated as group connections are added and deleted.
 */

struct fg_groups {
	struct fg_groups *next;
	struct connection *connection;
};

static struct fg_groups *groups = NULL;

/* Targets is a list of pairs: subnet and its policy group.
 * This list is bulk-updated on whack --listen and
 * incrementally updated when group connections are deleted.
 *
 * It is ordered by source subnet, and if those are equal, then target subnet.
 * A subnet is compared by comparing the network, and if those are equal,
 * comparing the mask.
 */

struct fg_targets {
	struct fg_targets *next;
	struct fg_groups *group;
	ip_subnet subnet;
	u_int8_t proto;
	u_int16_t startport;
	u_int16_t endport;
	char *name; /* name of instance of group conn */
};

static struct fg_targets *targets = NULL;

static struct fg_targets *new_targets;


/* subnetcmp compares the two ip_subnet values a and b.
 * It returns -1, 0, or +1 if a is, respectively,
 * less than, equal to, or greater than b.
 */
static int subnetcmp(const ip_subnet *a, const ip_subnet *b)
{
	ip_address neta, maska, netb, maskb;
	int r;

	networkof(a, &neta);
	maskof(a, &maska);
	networkof(b, &netb);
	maskof(b, &maskb);
	r = addrcmp(&neta, &netb);
	if (r == 0)
		r = addrcmp(&maska, &maskb);
	return r;
}

static void read_foodgroup(struct fg_groups *g)
{
	const char *fgn = g->connection->name;
	const ip_subnet *lsn = &g->connection->spd.this.client;
	const struct lsw_conf_options *oco = lsw_init_options();
	size_t plen = strlen(oco->policies_dir) + 2 + strlen(fgn) + 1;
	struct file_lex_position flp_space;

	/* food group should not specify leftprotoport/rightprotoport */
	if (g->connection->spd.this.protocol != 0 || g->connection->spd.this.port != 0 ||
		g->connection->spd.that.protocol != 0 || g->connection->spd.that.port != 0) {
		loglog(RC_LOG_SERIOUS, "Opportunistic template connection \"%s\" must not specify protoport. Instead configure protocol and ports in the policies/ files",
			g->connection->name);
		g->connection->spd.this.protocol = 0;
		g->connection->spd.this.port = 0;
		g->connection->spd.that.protocol = 0;
		g->connection->spd.that.port = 0;
	}

	if (plen > fg_path_space) {
		pfreeany(fg_path);
		fg_path_space = plen + 10;
		fg_path = alloc_bytes(fg_path_space, "policy group path");
	}
	snprintf(fg_path, fg_path_space, "%s/%s", oco->policies_dir, fgn);
	if (!lexopen(&flp_space, fg_path, TRUE)) {

		DBG(DBG_CONTROL, {
			    char cwd[PATH_MAX];
			    DBG_log("no group file \"%s\" (pwd:%s)",
				    fg_path,
				    getcwd(cwd, sizeof(cwd)));
		    });
	} else {
		libreswan_log("loading group \"%s\"", fg_path);
		for (;; ) {
			switch (flp->bdry) {
			case B_none:
			{
				const struct af_info *afi =
					strchr(flp->tok, ':') == NULL ?
					&af_inet4_info : &af_inet6_info;
				ip_subnet sn;
				u_int8_t proto = 0;
				u_int16_t startport = 0, endport = 0;
				bool has_port_wildcard;
				err_t ugh;

				if (strchr(flp->tok, '/') == NULL) {
					/* no /, so treat as /32 or V6 equivalent */
					ip_address t;

					ugh = ttoaddr_num(flp->tok, 0, afi->af,
						      &t);
					if (ugh == NULL)
						ugh = addrtosubnet(&t, &sn);
				} else {
					ugh = ttosubnet(flp->tok, 0, afi->af,
							&sn);
				}

				if (ugh != NULL) {
					loglog(RC_LOG_SERIOUS,
					       "\"%s\" line %d: %s \"%s\"",
					       flp->filename, flp->lino, ugh,
					       flp->tok);
				} else if ((afi->af != AF_INET) && (afi->af != AF_INET6)) {
					loglog(RC_LOG_SERIOUS,
					       "\"%s\" line %d: unsupported Address Family \"%s\"",
					       flp->filename, flp->lino,
					       flp->tok);
						(void)shift();
						flushline(NULL);
				} else {
					/* check for protocol and ports */
					libreswan_log("PAUL: checking proto ports on line %d", flp->lino);
					(void)shift();
					if (flp->bdry == B_none) {
						libreswan_log("PAUL: flp->bdry == B_none");
						//struct protoent *protoe;
						libreswan_log("PAUL: protocol is %s", flp->tok);

						ugh = ttoprotoport(flp->tok, 0, &proto, &startport, &has_port_wildcard);
						if (ugh == NULL) {
							libreswan_log("PAUL: converted to proto %d, port %d",
								proto, startport);
							endport = startport; /* range not supported yet */
						} else {
							libreswan_log("PAUL: protoport failed to convert: %s", ugh);
						}

					} else if (flp->bdry == B_record) {
						libreswan_log("PAUL: flp->bdry == B_record");
					} else if (flp->bdry == B_file) {
						libreswan_log("PAUL: flp->bdry == B_file");
					}
					flushline(NULL);


					/* Find where new entry ought to go in new_targets. */
					struct fg_targets **pp;
					int r;

					for (pp = &new_targets;;
					     pp = &(*pp)->next) {
						if (*pp == NULL) {
							r = -1; /* end of list is infinite */
							break;
						}
						r = subnetcmp(lsn,
							      &(*pp)->group->connection->spd.this.client);
						if (r == 0) {
							r = subnetcmp(&sn,
								      &(*pp)->subnet);
						}

						if (r <= 0)
							break;

						r = proto - (*pp)->proto;
						if (r != 0)
							break;

						r = startport - (*pp)->startport;
						if (r != 0)
							break;
						r = endport - (*pp)->endport;
						if (r != 0)
							break;
					}

					if (r == 0) {
						char source[SUBNETTOT_BUF];
						char dest[SUBNETTOT_BUF];

						subnettot(lsn, 0, source, sizeof(source));
						subnettot(&sn, 0, dest, sizeof(dest));
						loglog(RC_LOG_SERIOUS,
						       "\"%s\" line %d: subnet \"%s\", proto %d, port %d-%d, source %s, already \"%s\"",
						       flp->filename,
						       flp->lino,
						       dest,
						       proto, startport, endport,
						       source,
						       (*pp)->group->connection->name);
					} else {
						struct fg_targets *f =
							alloc_thing(
								struct fg_targets,
								"fg_target");

						f->next = *pp;
						f->group = g;
						f->subnet = sn;
						f->proto = proto;
						f->startport = startport;
						f->endport = endport;
						f->name = NULL;
						*pp = f;
						libreswan_log("PAUL: added %s", (*pp)->group->connection->name);
					}
				}
				//(void)shift(); /* next */
				//flushline("trailing characters after policy entry");
				continue;
			}

			case B_record:
				flp->bdry = B_none;     /* eat the Record Boundary */
				(void)shift();          /* get real first token */
				continue;

			case B_file:
				break;  /* done */
			}
			break;          /* if we reach here, out of loop */
		}
		lexclose();
	}
}

static void free_targets(void)
{
	while (targets != NULL) {
		struct fg_targets *t = targets;

		targets = t->next;
		pfreeany(t->name);
		pfree(t);
	}
}

void load_groups(void)
{
	passert(new_targets == NULL);

	/* for each group, add config file targets into new_targets */
	{
		struct fg_groups *g;

		for (g = groups; g != NULL; g = g->next)
			if (oriented(*g->connection))
				read_foodgroup(g);
	}

	/* dump new_targets */
	DBG(DBG_CONTROL,
	    {
		    struct fg_targets *t;

		    for (t = new_targets; t != NULL; t = t->next) {
			    char asource[SUBNETTOT_BUF];
			    char atarget[SUBNETTOT_BUF];

			    subnettot(&t->group->connection->spd.this.client,
				      0, asource, sizeof(asource));
			    subnettot(&t->subnet, 0, atarget, sizeof(atarget));
			    DBG_log("%s->%s %d %d %s",
				    asource, atarget,
					t->proto, t->startport,
				    t->group->connection->name);
		    }
	    });

	/* determine and deal with differences between targets and new_targets.
	 * structured like a merge.
	 */
	{
		struct fg_targets *op = targets,
		*np = new_targets;

		while (op != NULL && np != NULL) {
			int r = subnetcmp(
				&op->group->connection->spd.this.client,
				&np->group->connection->spd.this.client);

			if (r == 0)
				r = subnetcmp(&op->subnet, &np->subnet);
			if (r == 0)
				r = op->proto - np->proto;
			if (r == 0)
				r = op->startport = np->startport;
			if ( r== 0)
				r = op->endport = np->endport;

			if (r == 0 && op->group == np->group) {
				/* unchanged -- steal name & skip over */
				np->name = op->name;
				op->name = NULL;
				op = op->next;
				np = np->next;
			} else {
				/* note: following cases overlap! */
				if (r <= 0) {
					remove_group_instance(
						op->group->connection,
						op->name);
					op = op->next;
				}
				if (r >= 0) {
					np->name = add_group_instance(
						np->group->connection,
						&np->subnet, np->proto,
						np->startport, np->endport);
					np = np->next;
				}
			}
		}
		for (; op != NULL; op = op->next)
			remove_group_instance(op->group->connection, op->name);


		for (; np != NULL; np = np->next)
			np->name = add_group_instance(np->group->connection,
						      &np->subnet, np->proto,
						      np->startport, np->endport);

		/* update: new_targets replaces targets */
		free_targets();
		targets = new_targets;
		new_targets = NULL;
	}
}

void add_group(struct connection *c)
{
	struct fg_groups *g = alloc_thing(struct fg_groups, "policy group");

	g->next = groups;
	groups = g;

	g->connection = c;
}

static struct fg_groups *find_group(const struct connection *c)
{
	struct fg_groups *g;

	for (g = groups; g != NULL && g->connection != c; g = g->next)
		;
	return g;
}

void route_group(struct connection *c)
{
	/* it makes no sense to route a connection that is ISAKMP-only */
	if (!NEVER_NEGOTIATE(c->policy) && !HAS_IPSEC_POLICY(c->policy)) {
		loglog(RC_ROUTE,
		       "cannot route an ISAKMP-only group connection");
	} else {
		struct fg_groups *g = find_group(c);
		struct fg_targets *t;

		passert(g != NULL);
		g->connection->policy |= POLICY_GROUTED;
		for (t = targets; t != NULL; t = t->next) {
			if (t->group == g) {
				struct connection *ci = conn_by_name(t->name,
								    FALSE, FALSE);

				if (ci != NULL) {
					set_cur_connection(ci);
					if (!trap_connection(ci))
						whack_log(RC_ROUTE,
							  "could not route");
					set_cur_connection(c);
				}
			}
		}
	}
}

void unroute_group(struct connection *c)
{
	struct fg_groups *g = find_group(c);
	struct fg_targets *t;

	passert(g != NULL);
	g->connection->policy &= ~POLICY_GROUTED;
	for (t = targets; t != NULL; t = t->next) {
		if (t->group == g) {
			struct connection *ci = conn_by_name(t->name, FALSE, FALSE);

			if (ci != NULL) {
				set_cur_connection(ci);
				unroute_connection(ci);
				set_cur_connection(c);
			}
		}
	}
}

void delete_group(const struct connection *c)
{
	struct fg_groups *g;

	/* find and remove from groups */
	{
		struct fg_groups **pp;

		for (pp = &groups; (g = *pp)->connection != c;
		     pp = &(*pp)->next)
			;

		*pp = g->next;
	}

	/* find and remove from targets */
	{
		struct fg_targets **pp;

		for (pp = &targets; *pp != NULL; ) {
			struct fg_targets *t = *pp;

			if (t->group == g) {
				*pp = t->next;
				remove_group_instance(t->group->connection,
						      t->name);
				pfree(t);
				/* pp is ready for next iteration */
			} else {
				pp = &t->next;
			}
		}
	}

	pfree(g);
}
