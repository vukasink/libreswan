/* IKEv2 packet send routines, for Libreswan
 *
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2010,2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2017 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Sahana Prasad <sahana.prasad07@gmail.com>
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

#include "defs.h"

#include "send.h"
#include "ikev2_send.h"
#include "ikev2_message.h"
#include "server.h"
#include "state.h"
#include "connections.h"
#include "lswlog.h"
#include "ike_alg.h"
#include "pluto_stats.h"
#include "demux.h"	/* for struct msg_digest */
#include "rnd.h"

bool send_recorded_v2_ike_msg(struct state *st, const char *where)
{
	if (st->st_interface == NULL) {
		libreswan_log("Cannot send packet - interface vanished!");
		return false;
	} else if (st->st_v2_tfrags != NULL) {
		/* if a V2 packet needs fragmenting it would have already happened */
		passert(st->st_ike_version == IKEv2);
		passert(st->st_tpacket.ptr == NULL);
		unsigned nr_frags = 0;
		dbg("sending fragments ...");
		for (struct v2_ike_tfrag *frag = st->st_v2_tfrags;
		     frag != NULL; frag = frag->next) {
			if (!send_chunk_using_state(st, where, frag->cipher)) {
				dbg("send of fragment %u failed", nr_frags);
				return false;
			}
			nr_frags++;

		}
		dbg("sent %u fragments", nr_frags);
		return true;
	} else {
		return send_chunk_using_state(st, where, st->st_tpacket);
	}
}

/*
 * Send a payload.
 */

bool emit_v2UNKNOWN(const char *victim, pb_stream *outs)
{
	libreswan_log("IMPAIR: adding an unknown payload of type %d to %s",
		      ikev2_unknown_payload_desc.pt, victim);
	struct ikev2_generic gen = {
		.isag_np = ISAKMP_NEXT_v2NONE,
		.isag_critical = build_ikev2_critical(IMPAIR(UNKNOWN_PAYLOAD_CRITICAL)),
	};
	pb_stream pbs = open_output_struct_pbs(outs, &gen, &ikev2_unknown_payload_desc);
	if (!pbs_ok(&pbs)) {
		return false;
	}
	close_output_pbs(&pbs);
	return true;
}

/*
 * Send the STRING out as a V2 Vendor payload.
 *
 * XXX: Perhaps someday STRING will be replaced by enum
 * known_vendorid.
 */
bool emit_v2V(const char *string, pb_stream *outs)
{
	struct ikev2_generic gen = {
		.isag_np = 0,
	};
	pb_stream pbs = open_output_struct_pbs(outs, &gen,
					       &ikev2_vendor_id_desc);
	if (!pbs_ok(&pbs)) {
		return false;
	}
	if (!out_raw(string, strlen(string), &pbs, string)) {
		return false;
	}
	close_output_pbs(&pbs);
	return true;
}

/*
 * ship_v2N: add notify payload to the rbody
 * (See also specialized versions ship_v2Nsp and ship_v2Ns.)
 *
 * - RFC 7296 3.10 "Notify Payload" says:
 *
 * o  Protocol ID (1 octet) - If this notification concerns an existing
 *    SA whose SPI is given in the SPI field, this field indicates the
 *    type of that SA.  For notifications concerning Child SAs, this
 *    field MUST contain either (2) to indicate AH or (3) to indicate
 *    ESP.  Of the notifications defined in this document, the SPI is
 *    included only with INVALID_SELECTORS, REKEY_SA, and
 *    CHILD_SA_NOT_FOUND.  If the SPI field is empty, this field MUST be
 *    sent as zero and MUST be ignored on receipt.
 *
 * o  SPI Size (1 octet) - Length in octets of the SPI as defined by the
 *    IPsec protocol ID or zero if no SPI is applicable.  For a
 *    notification concerning the IKE SA, the SPI Size MUST be zero and
 *    the field must be empty.
 *
 *    Since all IKEv2 implementations MUST implement the NOTIFY type
 *    payload, these payloads NEVER have the Critical Flag set.
 */


/* emit a v2 Notification payload, with optional SA and optional sub-payload */
bool emit_v2Nsa_pl(v2_notification_t ntype,
		enum ikev2_sec_proto_id protoid,
		const ipsec_spi_t *spi, /* optional */
		pb_stream *outs,
		pb_stream *payload_pbs /* optional */)
{
	/* See RFC 5996 section 3.10 "Notify Payload" */
	passert(protoid == PROTO_v2_RESERVED || protoid == PROTO_v2_AH || protoid == PROTO_v2_ESP);

	switch (ntype) {
	case v2N_INVALID_SELECTORS:
	case v2N_REKEY_SA:
	case v2N_CHILD_SA_NOT_FOUND:
		if (protoid == PROTO_v2_RESERVED || spi == NULL) {
			dbg("XXX: type requires SA; missing");
		}
		break;
	default:
		if (protoid != PROTO_v2_RESERVED || spi != NULL) {
			dbg("XXX: type forbids SA but SA present");
		}
		break;
	}

	DBG(DBG_CONTROLMORE, DBG_log("Adding a v2N Payload"));

	struct ikev2_notify n = {
		.isan_critical = build_ikev2_critical(false),
		.isan_protoid = protoid,
		.isan_spisize = spi != NULL ? sizeof(*spi) : 0,
		.isan_type = ntype,
	};

	pb_stream pls;

	if (!out_struct(&n, &ikev2_notify_desc, outs, &pls) ||
	    (spi != NULL && !out_raw(spi, sizeof(*spi), &pls, "SPI"))) {
		return false;
	}

	if (payload_pbs == NULL)
		close_output_pbs(&pls);
	else
		*payload_pbs = pls;
	return true;
}

/* emit a v2 Notification payload, with optional sub-payload */
bool emit_v2Npl(v2_notification_t ntype,
		pb_stream *outs,
		pb_stream *payload_pbs /* optional */)
{
	return emit_v2Nsa_pl(ntype, PROTO_v2_RESERVED, NULL, outs, payload_pbs);
}

/* emit a v2 Notification payload, with optional chunk as sub-payload */
bool emit_v2Nchunk(v2_notification_t ntype,
		const chunk_t *ndata, /* optional */
		pb_stream *outs)
{
	pb_stream pl;

	if (!emit_v2Npl(ntype, outs, &pl) ||
	    (ndata != NULL && !out_chunk(*ndata, &pl, "Notify data")))
		return false;

	close_output_pbs(&pl);
	return true;
}

/* output a v2 simple Notification payload */
bool emit_v2N(v2_notification_t ntype,
	       pb_stream *outs)
{
	return emit_v2Npl(ntype, outs, NULL);
}

bool emit_v2N_signature_hash_algorithms(lset_t sighash_policy,
					pb_stream *outs)
{
	pb_stream n_pbs;

	if (!emit_v2Npl(v2N_SIGNATURE_HASH_ALGORITHMS, outs, &n_pbs)) {
		libreswan_log("error initializing notify payload for notify message");
		return false;
	}

#define H(POLICY, ID)							\
	if (sighash_policy & POLICY) {					\
		uint16_t hash_id = htons(ID);				\
		passert(sizeof(hash_id) == RFC_7427_HASH_ALGORITHM_IDENTIFIER_SIZE); \
		if (!out_raw(&hash_id, sizeof(hash_id), &n_pbs,		\
			     "hash algorithm identifier "#ID)) {	\
			return false;					\
		}							\
	}
	H(POL_SIGHASH_SHA2_256, IKEv2_AUTH_HASH_SHA2_256);
	H(POL_SIGHASH_SHA2_384, IKEv2_AUTH_HASH_SHA2_384);
	H(POL_SIGHASH_SHA2_512, IKEv2_AUTH_HASH_SHA2_512);
#undef H

	close_output_pbs(&n_pbs);
	return true;
}

/*
 *
 ***************************************************************
 *                       NOTIFICATION_OUT Complete packet  *****
 ***************************************************************
 *
 */

/*
 * This short/sharp notification is always tied to the IKE SA.
 *
 * For a CREATE_CHILD_SA, things have presumably screwed up so badly
 * that the larval child state is deleted.
 *
 * XXX: suspect calls to this function should be replaced by something
 * like record_v2N_spi_response_from_state() - so that the response is
 * always saved in the state and re-transmits can be handled
 * correctly.
 */

void send_v2N_spi_response_from_state(struct ike_sa *ike,
				      struct msg_digest *md,
				      enum ikev2_sec_proto_id protoid,
				      ipsec_spi_t *spi,
				      v2_notification_t ntype,
				      const chunk_t *ndata /* optional */)
{
	/*
	 * The caller must have computed DH and SKEYSEED; but may not
	 * have authenticated (i.e., don't assume that the IKE SA has
	 * "established").
	 */
	if (!pexpect(ike->sa.hidden_variables.st_skeyid_calculated)) {
		return;
	}

	passert(v2_msg_role(md) == MESSAGE_REQUEST); /* always responding */
	const char *const notify_name = enum_short_name(&ikev2_notify_names, ntype);

	enum isakmp_xchg_types exchange_type = md->hdr.isa_xchg;
	const char *const exchange_name = enum_short_name(&ikev2_exchange_names, exchange_type);

	ipstr_buf b;
	libreswan_log("responding to %s message (ID %u) from %s:%u with encrypted notification %s",
		      exchange_name, md->hdr.isa_msgid,
		      sensitive_ipstr(&ike->sa.st_remoteaddr, &b),
		      ike->sa.st_remoteport,
		      notify_name);

	/*
	 * For encrypted messages, the EXCHANGE TYPE can't be SA_INIT.
	 */
	switch (exchange_type) {
	case ISAKMP_v2_IKE_SA_INIT:
		PEXPECT_LOG("exchange type %s invalid for encrypted notification",
			    exchange_name);
		return;
	default:
		break;
	}

	uint8_t buf[MIN_OUTPUT_UDP_SIZE];
	pb_stream reply = open_out_pbs("encrypted notification",
				       buf, sizeof(buf));

	pb_stream rbody = open_v2_message(&reply, ike,
					  md /* response */,
					  exchange_type);
	if (!pbs_ok(&rbody)) {
		libreswan_log("error initializing hdr for encrypted notification");
		return;
	}

	v2SK_payload_t sk = open_v2SK_payload(&rbody, ike);
	if (!pbs_ok(&sk.pbs)) {
		return;
	}

	/* actual data */

	/*
	 * 3.10.  Notify Payload: Of the notifications defined in this
	 * document, the SPI is included only with INVALID_SELECTORS,
	 * REKEY_SA, and CHILD_SA_NOT_FOUND.
	*/
	switch (ntype) {
	case v2N_INVALID_SELECTORS:
		/*
		 * MAY be sent in an IKE INFORMATIONAL exchange when a
		 * node receives an ESP or AH packet whose selectors
		 * do not match those of the SA on which it was
		 * delivered (and that caused the packet to be
		 * dropped).  The Notification Data contains the start
		 * of the offending packet (as in ICMP messages) and
		 * the SPI field of the notification is set to match
		 * the SPI of the Child SA.
		*/
		PEXPECT_LOG("trying to send unimplemented %s notification",
			    notify_name);
		return;
	case v2N_REKEY_SA:
		PEXPECT_LOG("%s notification cannot be part of a response",
			    notify_name);
		return;
	default:
		break;
	}

	pb_stream n_pbs;
	if (!emit_v2Nsa_pl(ntype, protoid, spi, &sk.pbs, &n_pbs) ||
	    (ndata != NULL && !out_chunk(*ndata, &n_pbs, "Notify data"))) {
		return;
	}
	close_output_pbs(&n_pbs);

	if (!close_v2SK_payload(&sk)) {
		return;
	}
	close_output_pbs(&rbody);
	close_output_pbs(&reply);

	stf_status ret = encrypt_v2SK_payload(&sk);
	if (ret != STF_OK) {
		libreswan_log("error encrypting notify message");
		return;
	}

	/*
	 * The notification is piggybacked on the existing parent
	 * state.  This notification is fire-and-forget (not a proper
	 * exchange, one with retrying).  So we need not preserve the
	 * packet we are sending.
	 *
	 * XXX: this sounds wrong!  Integrity has been established so
	 * the outgoing packet should be retained and message counters
	 * updated.  If ST is going to be 'deleted', then, wouldn't it
	 * be better to have it linger a little so it can handle
	 * duplicates cleanly.
	 */
	send_chunk_using_state(&ike->sa, "v2 notify", same_out_pbs_as_chunk(&reply));
	pstat(ikev2_sent_notifies_e, ntype);
}

void send_v2N_response_from_state(struct ike_sa *ike,
				  struct msg_digest *md,
				  v2_notification_t ntype,
				  const chunk_t *ndata /* optional */)
{
	send_v2N_spi_response_from_state(ike, md, PROTO_v2_RESERVED, NULL/*SPI*/,
					 ntype, ndata);
}

/*
 * This is called with a pretty messed up MD so trust nothing.  For
 * instance when the version number is wrong.
 */
void send_v2N_response_from_md(struct msg_digest *md,
			       v2_notification_t ntype,
			       const chunk_t *ndata)
{
	passert(md != NULL); /* always a response */

	const char *const notify_name = enum_short_name(&ikev2_notify_names, ntype);
	passert(notify_name != NULL); /* must be known */

	enum isakmp_xchg_types exchange_type = md->hdr.isa_xchg;
	const char *exchange_name = enum_short_name(&ikev2_exchange_names, exchange_type);
	if (exchange_name == NULL) {
		/* when responding to crud, name may not be known */
		exchange_name = "UNKNOWN";
		dbg("message request contains unknown exchange type %d",
		    exchange_type);
	}

	ipstr_buf b;
	libreswan_log("responding to %s (%d) message (Message ID %u) from %s:%u with unencrypted notification %s",
		      exchange_name, exchange_type,
		      md->hdr.isa_msgid,
		      sensitive_ipstr(&md->sender, &b),
		      hportof(&md->sender),
		      notify_name);

	/*
	 * Normally an unencrypted response is only valid for
	 * IKE_SA_INIT or IKE_AUTH (when DH fails).  However "1.5.
	 * Informational Messages outside of an IKE SA" says to
	 * respond to other crud using the initiator's exchange type
	 * and Message ID and an unencrypted response.
	 */
	switch (exchange_type) {
	case ISAKMP_v2_IKE_SA_INIT:
	case ISAKMP_v2_IKE_AUTH:
		break;
	default:
		dbg("normally exchange type %s is encrypted", exchange_name);
	}

	uint8_t buf[MIN_OUTPUT_UDP_SIZE];
	pb_stream reply = open_out_pbs("unencrypted notification",
				       buf, sizeof(buf));
	pb_stream rbody = open_v2_message(&reply, NULL/*no state*/,
					  md /* response */,
					  exchange_type);
	if (!pbs_ok(&rbody)) {
		PEXPECT_LOG("error building header for unencrypted %s %s notification with message ID %u",
			    exchange_name, notify_name, md->hdr.isa_msgid);
		return;
	}

	/* build and add v2N payload to the packet */
	if (!emit_v2Nchunk(ntype, ndata, &rbody)) {
		PEXPECT_LOG("error building unencrypted %s %s notification with message ID %u",
			    exchange_name, notify_name, md->hdr.isa_msgid);
		return;
	}

	close_output_pbs(&rbody);
	close_output_pbs(&reply);

	/*
	 * This notification is fire-and-forget (not a proper
	 * exchange, one with retrying) so it is not saved.
	 */
	send_chunk("v2 notify", SOS_NOBODY, md->iface, md->sender,
		   same_out_pbs_as_chunk(&reply));

	pstat(ikev2_sent_notifies_e, ntype);
}

/*
 * Send an Informational Exchange announcing a deletion.
 *
 * CURRENTLY SUPPRESSED:
 * If we fail to send the deletion, we just go ahead with deleting the state.
 * The code in delete_state would break if we actually did this.
 *
 * Deleting an IKE SA is a bigger deal than deleting an IPsec SA.
 */

void record_v2_delete(struct state *const st)
{
	struct ike_sa *ike = ike_sa(st);
	if (ike == NULL) {
		/* ike_sa() will have already complained loudly */
		return;
	}

	/* make sure HDR is at start of a clean buffer */
	uint8_t buf[MIN_OUTPUT_UDP_SIZE];
	pb_stream packet = open_out_pbs("informational exchange delete request",
					buf, sizeof(buf));
	pb_stream rbody = open_v2_message(&packet, ike,
					  NULL /* request */,
					  ISAKMP_v2_INFORMATIONAL);
	if (!pbs_ok(&packet)) {
		return;
	}

	v2SK_payload_t sk = open_v2SK_payload(&rbody, ike);
	if (!pbs_ok(&sk.pbs)) {
		return;
	}

	{
		pb_stream del_pbs;
		struct ikev2_delete v2del_tmp;
		/*
		 * uint16_t i, j=0;
		 * u_char *spi;
		 * char spi_buf[1024];
		 */

		zero(&v2del_tmp);	/* OK: no pointer fields */
		v2del_tmp.isad_np = ISAKMP_NEXT_v2NONE;

		if (IS_CHILD_SA(st)) {
			v2del_tmp.isad_protoid = PROTO_IPSEC_ESP;
			v2del_tmp.isad_spisize = sizeof(ipsec_spi_t);
			v2del_tmp.isad_nrspi = 1;
		} else {
			v2del_tmp.isad_protoid = PROTO_ISAKMP;
			v2del_tmp.isad_spisize = 0;
			v2del_tmp.isad_nrspi = 0;
		}

		/* Emit delete payload header out */
		if (!out_struct(&v2del_tmp, &ikev2_delete_desc,
				&sk.pbs, &del_pbs))
			return;

		/* Emit values of spi to be sent to the peer */
		if (IS_CHILD_SA(st)) {
			if (!out_raw((u_char *)&st->st_esp.our_spi,
				     sizeof(ipsec_spi_t), &del_pbs,
				     "local spis"))
				return;
		}

		close_output_pbs(&del_pbs);
	}

	if (!close_v2SK_payload(&sk)) {
		return;
	}
	close_output_pbs(&rbody);
	close_output_pbs(&packet);

	stf_status ret = encrypt_v2SK_payload(&sk);
	if (ret != STF_OK) {
		libreswan_log("error encrypting notify message");
		return;
	}

	record_outbound_ike_msg(st, &packet, "packet for ikev2 delete informational");
}

/*
 * Construct and send an informational request.
 *
 * XXX: This and record_v2_delete() should be merged.  However, there
 * are annoying differences.  For instance, record_v2_delete() updates
 * st->st_msgid but the below doesn't.
 */
stf_status record_v2_informational_request(const char *name,
					   struct ike_sa *ike,
					   struct state *sender,
					   payload_master_t *payloads)
{
	/*
	 * Buffer in which to marshal our informational message.  We
	 * don't use reply_buffer/reply_stream because it might be in
	 * use.
	 */
	u_char buffer[MIN_OUTPUT_UDP_SIZE];	/* ??? large enough for any informational? */
	pb_stream packet = open_out_pbs(name, buffer, sizeof(buffer));
	if (!pbs_ok(&packet)) {
		return STF_INTERNAL_ERROR;
	}

	pb_stream message = open_v2_message(&packet, ike,
					    NULL /* request */,
					    ISAKMP_v2_INFORMATIONAL);
	if (!pbs_ok(&message)) {
		return STF_INTERNAL_ERROR;
	}

	v2SK_payload_t sk = open_v2SK_payload(&message, ike);
	if (!pbs_ok(&sk.pbs) ||
	    (payloads != NULL && !payloads(sender, &sk.pbs)) ||
	    !close_v2SK_payload(&sk)) {
		return STF_INTERNAL_ERROR;
	}
	close_output_pbs(&message);
	close_output_pbs(&packet);

	stf_status ret = encrypt_v2SK_payload(&sk);
	if (ret != STF_OK) {
		return ret;
	}

	/*
	 * cannot use ikev2_update_msgid_counters - no md here
	 *
	 * XXX: Record the outbound message in the CHILD SA.  There's
	 * code in record_outbound_ike_msg() that updates
	 * .st_last_liveness and if that's IKE then the IKE SA never
	 * times out - any message updates the IKE timer making the
	 * logic conclude that all is well!  Surely this is wrong!  Or
	 * perhaps this some sort of variable abuse where for CHILD SA
	 * it is last outgoing, and for IKE it is last incomming?
	 *
	 * XXX: If this is the IKE SA responder sending out a request
	 * (so Message ID is 0) things get really screwed up.  The
	 * first response (Message ID is 0) will be dropped because
	 * the message routing code will find the CHILD SA (it has a
	 * default msgid==0) and then discover that there's no
	 * handler.  The second and further responses will make it
	 * through, but only because there's fall back logic to route
	 * the message to the IKE SA when there's no child.
	 * Hopefully, by the time you read this, these bugs will have
	 * been fixed.
	 */
	msgid_t new_msgid = ike->sa.st_msgid_nextuse;
	msgid_t new_nextuse = ike->sa.st_msgid_nextuse + 1;
	dbg("Message ID: IKE #%lu sender #%lu in %s record 'n' send notify request so forcing IKE nextuse="PRI_MSGID"->"PRI_MSGID" and IKE msgid="PRI_MSGID"->"PRI_MSGID,
	    ike->sa.st_serialno, sender->st_serialno, __func__,
	    ike->sa.st_msgid_nextuse, new_nextuse,
	    ike->sa.st_msgid, new_msgid);
	ike->sa.st_msgid_nextuse = new_nextuse;
	ike->sa.st_msgid = new_msgid;

	ike->sa.st_pend_liveness = TRUE; /* we should only do this when dpd/liveness is active? */
	record_outbound_ike_msg(sender, &packet, name);
	return STF_OK;
}