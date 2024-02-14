/* do PSK operations for IKEv2
 *
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2008-2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2015 Antony Antony <antony@phenome.org>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2015 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
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

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pk11pub.h>

#include "sysdep.h"
#include "constants.h"

#include "defs.h"
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
#include "ikev2.h"
#include "server.h"
#include "keys.h"
#include "crypt_prf.h"
#include "crypt_symkey.h"
#include "fips_mode.h"
#include "ikev2_prf.h"
#include "ikev2_psk.h"

diag_t ikev2_calculate_psk_sighash(enum perspective perspective,
				   const struct hash_signature *auth_sig,
				   const struct ike_sa *ike,
				   enum keyword_auth authby,
				   const struct crypt_mac *idhash,
				   const chunk_t firstpacket,
				   struct crypt_mac *sighash)
{
	const struct connection *c = ike->sa.st_connection;
	*sighash = empty_mac;
	passert(authby == AUTH_EAPONLY || authby == AUTH_PSK || authby == AUTH_NULL);

	enum_buf pb;
	ldbg_sa(ike, "%s() called for %s to %s PSK with authby=%s",
		__func__, ike->sa.st_state->name,
		str_enum_short(&perspective_names, perspective, &pb),
		enum_name(&keyword_auth_names, authby));

	/* this is the IKE_AUTH exchange, so a given */
	passert(ike->sa.hidden_variables.st_skeyid_calculated);

	chunk_t intermediate_auth = empty_chunk;
	if (ike->sa.st_v2_ike_intermediate.enabled) {
		intermediate_auth = clone_hunk_hunk(ike->sa.st_v2_ike_intermediate.initiator,
						    ike->sa.st_v2_ike_intermediate.responder,
						    "IntAuth_*_I_A | IntAuth_*_R");
		/* IKE AUTH's first Message ID */
		uint8_t ike_auth_mid[sizeof(ike->sa.st_v2_ike_intermediate.id)];
		hton_bytes(ike->sa.st_v2_ike_intermediate.id + 1,
			   ike_auth_mid, sizeof(ike_auth_mid));
		append_chunk_thing("IKE_AUTH_MID", &intermediate_auth, ike_auth_mid);
	}

	/*
	 * Pick nullauth_pss, nonce, and nonce_name suitable for
	 * (state, verify).
	 *
	 * RFC-7619:
	 *
	 * When using the NULL Authentication Method, the content of
	 * the AUTH payload is computed using the syntax of pre-shared
	 * secret authentication, described in Section 2.15 of
	 * [RFC7296].  The values SK_pi and SK_pr are used as shared
	 * secrets for the content of the AUTH payloads generated by
	 * the initiator and the responder respectively.
	 *
	 * We have SK_pi/SK_pr as PK11SymKey in st_skey_pi_nss and
	 * st_skey_pr_nss.
	 */

	chunk_t nullauth_pss;
	const chunk_t *nonce;
	const char *nonce_name;

	switch (ike->sa.st_sa_role) {
	case SA_INITIATOR:
		switch (perspective) {
		case REMOTE_PERSPECTIVE:
			/* we are initiator verifying PSK */
			nonce_name = "Ni: initiator re-generating hash from responder";
			nonce = &ike->sa.st_ni;
			nullauth_pss = ike->sa.st_skey_chunk_SK_pr;
			break;
		case LOCAL_PERSPECTIVE:
			/* we are initiator sending PSK */
			nonce_name = "Nr: initiator generating hash for responder";
			nonce = &ike->sa.st_nr;
			nullauth_pss = ike->sa.st_skey_chunk_SK_pi;
			break;
		case NO_PERSPECTIVE:
		default:
			bad_case(perspective);
		}
		break;

	case SA_RESPONDER:
		switch (perspective) {
		case REMOTE_PERSPECTIVE:
			/* we are responder verifying PSK */
			nonce_name = "Nr: responder re-generating hash from initiator";
			nonce = &ike->sa.st_nr;
			nullauth_pss = ike->sa.st_skey_chunk_SK_pi;
			break;
		case LOCAL_PERSPECTIVE:
			/* we are responder sending PSK */
			nonce_name = "Ni: create: responder generating hash for initiator";
			nonce = &ike->sa.st_ni;
			nullauth_pss = ike->sa.st_skey_chunk_SK_pr;
			break;
		case NO_PERSPECTIVE:
		default:
			bad_case(perspective);
		}
		break;

	default:
		bad_case(ike->sa.st_sa_role);
	}

	/* pick pss */

	shunk_t pss;

	if (auth_sig != NULL) {
		/*
		 * Use the auth sig passed in, presumably accumulated
		 * by repeated EAP-TLS exchanges?
		 */
		if (!pexpect(auth_sig->len > 0)) {
			return diag(PEXPECT_PREFIX"missing auth_sig");
		}
		pss = HUNK_AS_SHUNK(*auth_sig);
	} else if (authby != AUTH_NULL) {
		/*
		 * XXX: same PSK used for both local and remote end,
		 * so peer doesn't apply?
		 */
		const chunk_t *psk = get_connection_psk(c);
		if (psk == NULL) {
			id_buf idb;
			return diag("authentication failed: no PSK found for '%s'",
				    str_id(&c->local->host.id, &idb));
		}

		/* XXX: this should happen during connection load */
		const size_t key_size_min = crypt_prf_fips_key_size_min(ike->sa.st_oakley.ta_prf);
		if (psk->len < key_size_min) {
			if (is_fips_mode()) {
				id_buf idb;
				return diag("FIPS: authentication failed: '%s' PSK length of %zu bytes is too short for PRF %s in FIPS mode (%zu bytes required)",
					    str_id(&c->local->host.id, &idb),
					    psk->len,
					    ike->sa.st_oakley.ta_prf->common.fqn,
					    key_size_min);
			}

			id_buf idb;
			llog_sa(RC_LOG, ike,
				  "WARNING: '%s' PSK length of %zu bytes is too short for PRF %s in FIPS mode (%zu bytes required)",
				  str_id(&c->local->host.id, &idb),
				  psk->len,
				  ike->sa.st_oakley.ta_prf->common.fqn,
				  key_size_min);
		}
		pss = HUNK_AS_SHUNK(*psk);
	} else {
		pss = HUNK_AS_SHUNK(nullauth_pss);
	}

	passert(pss.len != 0);

	if (DBGP(DBG_CRYPT)) {
		DBG_dump_hunk("inputs to hash1 (first packet)", firstpacket);
		DBG_dump_hunk(nonce_name, *nonce);
		DBG_dump_hunk("idhash", *idhash);
		DBG_dump_hunk("IntAuth", intermediate_auth);
		DBG_dump_hunk("PSK", pss);
	}

	/*
	 * RFC 4306 2.15:
	 * AUTH = prf(prf(Shared Secret, "Key Pad for IKEv2"), <msg octets>)
	 */
	passert(idhash->len == ike->sa.st_oakley.ta_prf->prf_output_size);
	*sighash = ikev2_psk_auth(ike->sa.st_oakley.ta_prf, pss,
				  firstpacket, *nonce, idhash,
				  intermediate_auth, ike->sa.logger);
	free_chunk_content(&intermediate_auth);
	return NULL;
}

bool ikev2_create_psk_auth(enum keyword_auth authby,
			   const struct ike_sa *ike,
			   const struct crypt_mac *idhash,
			   chunk_t *additional_auth /* output */)
{
	*additional_auth = empty_chunk;
	struct crypt_mac signed_octets = empty_mac;
	diag_t d = ikev2_calculate_psk_sighash(LOCAL_PERSPECTIVE, NULL,
					       ike, authby, idhash,
					       ike->sa.st_firstpacket_me,
					       &signed_octets);
	if (d != NULL) {
		llog_diag(RC_LOG_SERIOUS, ike->sa.logger, &d, "%s", "");
		return false;
	}

	const char *chunk_n = (authby == AUTH_PSK) ? "NO_PPK_AUTH chunk" : "NULL_AUTH chunk";
	*additional_auth = clone_hunk(signed_octets, chunk_n);
	if (DBGP(DBG_CRYPT)) {
		DBG_dump_hunk(chunk_n, *additional_auth);
	}

	return true;
}

/*
 * Check the signature using PSK authentication and log the outcome.
 *
 * The log message must mention both the peer's ID and kind.
 */

diag_t verify_v2AUTH_and_log_using_psk(enum keyword_auth authby,
				       const struct ike_sa *ike,
				       const struct crypt_mac *idhash,
				       struct pbs_in *sig_pbs,
				       const struct hash_signature *auth_sig)
{
	shunk_t sig = pbs_in_left(sig_pbs);

	passert(authby == AUTH_EAPONLY || authby == AUTH_PSK || authby == AUTH_NULL);

	size_t hash_len = ike->sa.st_oakley.ta_prf->prf_output_size;
	if (sig.len != hash_len) {
		esb_buf kb;
		id_buf idb;
		return diag("authentication failed: %zu byte hash received from peer %s '%s' does not match %zu byte hash of negotiated PRF %s",
			    sig.len,
			    enum_show(&ike_id_type_names, ike->sa.st_connection->remote->host.id.kind, &kb),
			    str_id(&ike->sa.st_connection->remote->host.id, &idb),
			    hash_len, ike->sa.st_oakley.ta_prf->common.fqn);
	}

	struct crypt_mac calc_hash = empty_mac;
	diag_t d = ikev2_calculate_psk_sighash(REMOTE_PERSPECTIVE, auth_sig,
					       ike, authby, idhash,
					       ike->sa.st_firstpacket_peer,
					       &calc_hash);
	if (d != NULL) {
		return d;
	}

	if (DBGP(DBG_CRYPT)) {
	    DBG_dump_hunk("Received PSK auth octets", sig);
	    DBG_dump_hunk("Calculated PSK auth octets", calc_hash);
	}

	if (!hunk_eq(sig, calc_hash)) {
		id_buf idb;
		esb_buf kb;
		return diag("authentication failed: computed hash does not match hash received from peer %s '%s'",
			    enum_show(&ike_id_type_names, ike->sa.st_connection->remote->host.id.kind, &kb),
			    str_id(&ike->sa.st_connection->remote->host.id, &idb));
	}

	LLOG_JAMBUF(RC_LOG_SERIOUS, ike->sa.logger, buf) {
		jam(buf, "%s established IKE SA; ",
		    (ike->sa.st_sa_role == SA_INITIATOR ? "initiator" :
		     ike->sa.st_sa_role == SA_RESPONDER ? "responder" :
		     "?"));
		/* all methods log this string */
		jam_string(buf, "authenticated peer ");
		/* what was in the AUTH payload */
		/* XXX: log prf(prf(hash based on null or secret)) how? */
		/* now it was authenticated */
		jam_string(buf, "using authby=");
		jam_enum(buf, &keyword_auth_names, authby);
		jam_string(buf, " and ");
		jam_enum(buf, &ike_id_type_names, ike->sa.st_connection->remote->host.id.kind);
		jam_string(buf, " '");
		jam_id_bytes(buf, &ike->sa.st_connection->remote->host.id, jam_raw_bytes);
		jam_string(buf, "'");
	}
	return NULL;
}
