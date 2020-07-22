/* ip port (port), for libreswan
 *
 * Copyright (C) 2020 Andrew Cagney
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
 */

#include <arpa/inet.h>		/* for ntohs() */

#include "ip_port.h"

ip_port unset_port; /* aka all ports? */

ip_port ip_hport(unsigned hport)
{
	ip_port port = { .hport = hport, };
	return port;
}

ip_port ip_nport(unsigned nport)
{
	return ip_hport(ntohs(nport));
}

unsigned hport(const ip_port port)
{
	return port.hport;
}

unsigned nport(const ip_port port)
{
	return htons(port.hport);
}

bool port_is_unset(ip_port port)
{
	return port.hport == 0;
}

size_t jam_hport(jambuf_t *buf, ip_port port)
{
	return jam(buf, PRI_HPORT, hport(port));

}

size_t jam_nport(jambuf_t *buf, ip_port port)
{
	return jam(buf, PRI_NPORT, pri_nport(port));
}

const char *str_hport(ip_port port, port_buf *buf)
{
	jambuf_t jambuf = ARRAY_AS_JAMBUF(buf->buf);
	jam_hport(&jambuf, port);
	return buf->buf;
}

const char *str_nport(ip_port port, port_buf *buf)
{
	jambuf_t jambuf = ARRAY_AS_JAMBUF(buf->buf);
	jam_nport(&jambuf, port);
	return buf->buf;
}
