/* test IP code, for libreswan
 *
 * Copyright (C) 2000  Henry Spencer.
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2018 Andrew Cagney
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 *
 */

#include <stdio.h>

#include "lswlog.h" /* for log_ip */
#include "constants.h"
#include "ip_address.h"
#include "stdlib.h"
#include "ipcheck.h"

unsigned fails;
bool use_dns = true;

int main(int argc, char *argv[])
{
	log_ip = false; /* force sensitive */

	for (char **argp = argv+1; argp < argv+argc; argp++) {
		if (streq(*argp, "--nodns")) {
			use_dns = false;
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n",
				argv[0], *argp);
			return 1;
		}
	}

	ip_address_check();
	ip_endpoint_check();
	ip_range_check();
	ip_subnet_check();
	ip_said_check();
	ip_info_check();
	ip_protoport_check();
	ip_selector_check();
	ip_sockaddr_check();
	ip_port_check();
	ip_port_range_check();

	if (fails > 0) {
		fprintf(stderr, "TOTAL FAILURES: %d\n", fails);
		return 1;
	} else {
		return 0;
	}
}
