/* do PSK operations for IKEv2
 *
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2008-2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2015 Antony Antony <antony@phenome.org>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2015 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2015, 2017 Andrew Cagney
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libreswan.h>

#include "sysdep.h"
#include "constants.h"
#include "lswlog.h"

#include "defs.h"
#include "cookie.h"
#include "id.h"
#include "x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "crypto.h"
#include "ike_alg.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "pluto_crypt.h"  /* for pluto_crypto_req & pluto_crypto_req_cont */
#include "ikev2.h"
#include "server.h"
#include "vendor.h"
#include "keys.h"
#include "crypt_prf.h"
#include "crypt_symkey.h"
#include "lswfips.h"

#include <nss.h>
#include <pk11pub.h>

static const char psk_key_pad_str[] = "Key Pad for IKEv2";  /* 4306  2:15 */
static const size_t psk_key_pad_str_len = 17;                  /* sizeof(psk_key_pad_str); -1 */

static bool ikev2_calculate_psk_sighash(bool verify, struct state *st,
					enum keyword_authby authby,
					unsigned char *idhash,
					chunk_t firstpacket,
					unsigned char *signed_octets)
{
	const struct connection *c = st->st_connection;
	const chunk_t *pss = &empty_chunk;
	const size_t hash_len =  st->st_oakley.ta_prf->prf_output_size;

	passert(authby == AUTH_PSK || authby == AUTH_NULL);

	DBG(DBG_CONTROL,DBG_log("ikev2_calculate_psk_sighash() called from %s to %s PSK with authby=%s",
		st->st_state_name,
		verify ? "verify" : "create",
		enum_name(&ikev2_asym_auth_name, authby)));

	if (authby != AUTH_NULL) {
		pss = get_psk(c);
		if (pss == NULL) {
			libreswan_log("No matching PSK found for connection:%s",
			      st->st_connection->name);
			return FALSE; /* failure: no PSK to use */
		}
		DBG(DBG_PRIVATE, DBG_dump_chunk("User PSK:", *pss));
		const size_t key_size_min = crypt_prf_fips_key_size_min(st->st_oakley.ta_prf);
		if (pss->len < key_size_min) {
			if (libreswan_fipsmode()) {
				loglog(RC_LOG_SERIOUS,
				       "FIPS: connection %s PSK length of %zu bytes is too short for %s PRF in FIPS mode (%zu bytes required)",
				       st->st_connection->name,
				       pss->len,
				       st->st_oakley.ta_prf->common.name,
				       key_size_min);
				return FALSE;
			} else {
				libreswan_log("WARNING: connection %s PSK length of %zu bytes is too short for %s PRF in FIPS mode (%zu bytes required)",
					      st->st_connection->name,
					      pss->len,
					      st->st_oakley.ta_prf->common.name,
					      key_size_min);
			}
		}
	} else {
		/*
		 * RFC-7619
		 *
		 * When using the NULL Authentication Method, the
		 * content of the AUTH payload is computed using the
		 * syntax of pre-shared secret authentication,
		 * described in Section 2.15 of [RFC7296].  The values
		 * SK_pi and SK_pr are used as shared secrets for the
		 * content of the AUTH payloads generated by the
		 * initiator and the responder respectively.
		 *
		 * We have SK_pi/SK_pr as PK11SymKey in st_skey_pi_nss
		 * and st_skey_pr_nss
		 */
		passert(st->hidden_variables.st_skeyid_calculated);

		if (st->st_state == STATE_PARENT_I2 && !verify) {
			/* we are initiator sending PSK */
			pss = &st->st_skey_chunk_SK_pi;
		} else if ((st->st_state == STATE_PARENT_I2 ||
				st->st_state == STATE_PARENT_I3) && verify) {
			/* we are initiator verifying PSK */
			pss = &st->st_skey_chunk_SK_pr;
		} else if (st->st_state == STATE_PARENT_R2 && !verify) {
			/* we are responder sending PSK */
			pss = &st->st_skey_chunk_SK_pr;
		} else if (st->st_state == STATE_PARENT_R1 && verify) {
			/* we are responder verifying PSK */
			pss = &st->st_skey_chunk_SK_pi;
		}

		 DBG(DBG_PRIVATE, DBG_dump_chunk("AUTH_NULL PSK:", *pss));
	}

	passert(pss->len != 0);

	/*
	 * RFC 4306 2.15:
	 * AUTH = prf(prf(Shared Secret,"Key Pad for IKEv2"), <msg octets>)
	 */

	/* calculate inner prf */
	PK11SymKey *prf_psk;
	{
		struct crypt_prf *prf =
			crypt_prf_init_chunk("<prf-psk> = prf(<psk>,\"Key Pad for IKEv2\")",
					     DBG_CRYPT,
					     st->st_oakley.ta_prf,
					     "shared secret", *pss);
		if (prf == NULL) {
			if (libreswan_fipsmode()) {
				PASSERT_FAIL("FIPS: failure creating %s PRF context for digesting PSK",
					     st->st_oakley.ta_prf->common.name);
			}
			loglog(RC_LOG_SERIOUS,
			       "failure creating %s PRF context for digesting PSK",
			       st->st_oakley.ta_prf->common.name);
			return FALSE;
		}
		crypt_prf_update_bytes(psk_key_pad_str/*name*/, prf,
				       psk_key_pad_str, psk_key_pad_str_len);
		prf_psk = crypt_prf_final_symkey(&prf);
	}

	/* pick nonce */
	const chunk_t *nonce = NULL;
	const char *nonce_name = NULL;

	if (st->st_state == STATE_PARENT_I2 && !verify) {
		/* we are initiator sending PSK */
		nonce = &st->st_nr;
		nonce_name = "create: initiator inputs to hash2 (responder nonce)";
	} else if ((st->st_state == STATE_PARENT_I2 ||
			st->st_state == STATE_PARENT_I3) && verify) {
		/* we are initiator verifying PSK */
		nonce = &st->st_ni;
		nonce_name = "verify: initiator inputs to hash2 (initiator nonce)";
	} else if (st->st_state == STATE_PARENT_R2 && !verify) {
		/* we are responder sending PSK */
		nonce = &st->st_ni;
		nonce_name = "create: responder inputs to hash2 (initiator nonce)";
	} else if (st->st_state == STATE_PARENT_R1 && verify) {
		/* we are responder verifying PSK */
		nonce = &st->st_nr;
		nonce_name = "verify: initiator inputs to hash2 (responder nonce)";
	}

	passert(nonce != NULL);	/* we NEED a nonce */

	/* calculate outer prf */
	{
		struct crypt_prf *prf =
			crypt_prf_init_symkey("<signed-octets> = prf(<prf-psk>, <msg octets>)",
					      DBG_CRYPT, st->st_oakley.ta_prf,
					      "<prf-psk>", prf_psk);
		/*
		 * For the responder, the octets to be signed start
		 * with the first octet of the first SPI in the header
		 * of the second message and end with the last octet
		 * of the last payload in the second message.
		 * Appended to this (for purposes of computing the
		 * signature) are the initiator's nonce Ni (just the
		 * value, not the payload containing it), and the
		 * value prf(SK_pr,IDr') where IDr' is the responder's
		 * ID payload excluding the fixed header.  Note that
		 * neither the nonce Ni nor the value prf(SK_pr,IDr')
		 * are transmitted.
		 */
		crypt_prf_update_chunk("first-packet", prf, firstpacket);
		crypt_prf_update_chunk("nonce", prf, *nonce);
		crypt_prf_update_bytes("hash", prf, idhash, hash_len);
		crypt_prf_final_bytes(&prf, signed_octets, hash_len);
	}
	release_symkey(__func__, "prf-psk", &prf_psk);

	DBG(DBG_CRYPT,
	    DBG_dump_chunk("inputs to hash1 (first packet)", firstpacket);
	    DBG_dump_chunk(nonce_name, *nonce);
	    DBG_dump("idhash", idhash, hash_len));

	return TRUE;
}

bool ikev2_create_psk_auth(enum keyword_authby authby,
			      struct state *st,
			      unsigned char *idhash,
			      pb_stream *a_pbs,
			      bool calc_no_ppk_auth,
			      chunk_t *no_ppk_auth)
{
	unsigned int hash_len = st->st_oakley.ta_prf->prf_output_size;
	unsigned char signed_octets[hash_len];
	/* used to generate _and_ verify PSK, so use state kind */

	passert(authby == AUTH_PSK || authby == AUTH_NULL);

	if (!ikev2_calculate_psk_sighash(FALSE, st, authby, idhash,
				st->st_firstpacket_me,
				signed_octets)) {
		return FALSE;
	}

	DBG(DBG_PRIVATE,
	    DBG_dump("PSK auth octets", signed_octets, hash_len));

	if (calc_no_ppk_auth == FALSE) {
		if (!out_raw(signed_octets, hash_len, a_pbs, "PSK auth"))
			return FALSE;
	} else {
		clonetochunk(*no_ppk_auth, signed_octets, hash_len, "NO_PPK_AUTH chunk");
		DBG(DBG_PRIVATE, DBG_dump_chunk("NO_PPK_AUTH payload", *no_ppk_auth));
	}

	return TRUE;
}

stf_status ikev2_verify_psk_auth(enum keyword_authby authby,
				 struct state *st,
				 unsigned char *idhash,
				 pb_stream *sig_pbs)
{
	unsigned int hash_len = st->st_oakley.ta_prf->prf_output_size;
	unsigned char calc_hash[hash_len];
	size_t sig_len = pbs_left(sig_pbs);

	passert(authby == AUTH_PSK || authby == AUTH_NULL);

	if (sig_len != hash_len) {
		libreswan_log("negotiated prf: %s ",
			      st->st_oakley.ta_prf->common.name);
		libreswan_log(
			"I2 hash length:%lu does not match with PRF hash len %lu",
			(long unsigned) sig_len,
			(long unsigned) hash_len);
		return STF_FAIL;
	}


	if (!ikev2_calculate_psk_sighash(TRUE, st, authby, idhash,
					 st->st_firstpacket_him, calc_hash)) {
		return STF_FAIL;
	}

	DBG(DBG_PRIVATE,
	    DBG_dump("Received PSK auth octets", sig_pbs->cur, sig_len);
	    DBG_dump("Calculated PSK auth octets", calc_hash, hash_len));

	if (memeq(sig_pbs->cur, calc_hash, hash_len) ) {
		loglog(RC_LOG_SERIOUS, "Authenticated using %s",
			authby == AUTH_NULL ? "authby=null" : "authby=secret");
		return STF_OK;
	} else {
		loglog(RC_LOG_SERIOUS, "AUTH mismatch: Received AUTH != computed AUTH");
		return STF_FAIL;
	}
}
