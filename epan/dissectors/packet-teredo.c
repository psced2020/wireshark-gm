/* packet-teredo.c  v.1.0
 * Routines for Teredo packets disassembly
 *   draft-huitema-v6ops-teredo-02.txt
 *
 * Copyright 2003, Ragi BEJJANI - 6WIND - <ragi.bejjani@6wind.com>
 * Copyright 2003, Vincent JARDIN - 6WIND - <vincent.jardin@6wind.com>
 * Copyright 2004, Remi DENIS-COURMONT
 *
 * $Id$
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <epan/packet.h>
#include <epan/addr_resolv.h>
#include "ipproto.h"
#include "prefs.h"

#include "packet-ip.h"
#include "tap.h"

#define UDP_PORT_TEREDO 3544

static int teredo_tap = -1;

static int proto_teredo = -1;

static int hf_teredo_auth = -1;
static int hf_teredo_auth_idlen = -1;
static int hf_teredo_auth_aulen = -1;
static int hf_teredo_auth_id = -1;
static int hf_teredo_auth_value = -1;
static int hf_teredo_auth_nonce = -1;
static int hf_teredo_auth_conf = -1;
static int hf_teredo_orig = -1;
static int hf_teredo_orig_port = -1;
static int hf_teredo_orig_addr = -1;

static gint ett_teredo = -1;
static gint ett_teredo_auth = -1, ett_teredo_orig = -1;

typedef struct {
	guint16 th_indtyp;
	guint8 th_cidlen;
	guint8 th_authdlen;
	guint8 th_nonce[8];
	guint8 th_conf; 

	guint8 th_ip_v_hl;  
	guint16 th_header;
	guint16 th_orgport;
	guint32 th_iporgaddr;
} e_teredohdr;

static dissector_table_t teredo_dissector_table;
/*static heur_dissector_list_t heur_subdissector_list;*/
static dissector_handle_t data_handle;

static int
parse_teredo_auth(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
			int offset, e_teredohdr *teredoh)
{
	unsigned idlen, aulen;

	if (check_col(pinfo->cinfo, COL_INFO))
		col_append_sep_str (pinfo->cinfo, COL_INFO, ", ",
					"Authentication header");

	teredoh->th_indtyp = 1;
	offset += 2;

	idlen = tvb_get_guint8(tvb, offset);
	teredoh->th_cidlen = idlen;
	offset++;

	aulen = tvb_get_guint8(tvb, offset);
	teredoh->th_authdlen = aulen;
	offset++;

	if (tree) {
		proto_item *ti;

		ti = proto_tree_add_item(tree, hf_teredo_auth, tvb, offset-4,
						13 + idlen + aulen, FALSE);
		tree = proto_item_add_subtree(ti, ett_teredo_auth);
	
		proto_tree_add_item(tree, hf_teredo_auth_idlen, tvb,
					offset - 2, 1, FALSE);
		proto_tree_add_item(tree, hf_teredo_auth_aulen, tvb,
					offset - 1, 1, FALSE);

		/* idlen is usually zero */
		if (idlen) {
			proto_tree_add_item(tree, hf_teredo_auth_id, tvb,
						offset, idlen, FALSE);
			offset += idlen;
		}

		/* aulen is usually zero */
		if (aulen) {
			proto_tree_add_item(tree, hf_teredo_auth_value, tvb,
						offset, aulen, FALSE);
			offset += aulen;
		}

		proto_tree_add_item(tree, hf_teredo_auth_nonce, tvb,
					offset, 8, FALSE);
		offset += 8;

		proto_tree_add_item(tree, hf_teredo_auth_conf, tvb,
					offset, 1, FALSE);
		offset++;
	}
	else
		offset += idlen + aulen + 9;

	tvb_memcpy(tvb, teredoh->th_nonce, offset - 9, 8);
	teredoh->th_conf = tvb_get_guint8(tvb, offset - 1);

	return offset;
}


static int
parse_teredo_orig(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
			int offset, e_teredohdr *teredoh)
{
	if (check_col(pinfo->cinfo, COL_INFO))
		col_append_sep_str (pinfo->cinfo, COL_INFO, ", ",
					"Origin indication");

	if (tree) {
		proto_item *ti;
		guint16 port;
		guint32 addr;

		ti = proto_tree_add_item(tree, hf_teredo_orig, tvb, offset,
						8, FALSE);
		tree = proto_item_add_subtree(ti, ett_teredo_orig);
		offset += 2;

		port = ~tvb_get_ntohs(tvb, offset);
		proto_tree_add_uint(tree, hf_teredo_orig_port, tvb,
					offset, 2, port);
		offset += 2;

		tvb_memcpy(tvb, (guint8 *)&addr, offset, 4);
		proto_tree_add_ipv4(tree, hf_teredo_orig_addr, tvb,
					offset, 4, ~addr);
		offset += 4;
	}
	else
		offset += 8;

	teredoh->th_orgport = tvb_get_ntohs(tvb, offset - 6);
	tvb_memcpy(tvb, (guint8 *)&teredoh->th_iporgaddr, offset-4, 4);

	return offset;
}


/* Determine if there is a sub-dissector and call it.  This has been */
/* separated into a stand alone routine to other protocol dissectors */
/* can call to it, ie. socks	*/


static void
decode_teredo_ports(tvbuff_t *tvb, int offset, packet_info *pinfo,proto_tree *tree, int th_header)
{
	tvbuff_t *next_tvb;

	next_tvb = tvb_new_subset(tvb, offset, -1, -1);

	if (dissector_try_port(teredo_dissector_table, th_header, next_tvb, pinfo, tree))
		return;

	call_dissector(data_handle,next_tvb, pinfo, tree);  
}

static void
dissect_teredo(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	proto_tree *teredo_tree;
	proto_item *ti;
	int        offset = 0;
	static e_teredohdr teredohstruct[4], *teredoh;
	static int teredoh_count = 0;

	teredoh_count++;
	if(teredoh_count>=4){
		teredoh_count=0;
	}
	teredoh = &teredohstruct[teredoh_count];

	if (check_col(pinfo->cinfo, COL_PROTOCOL))
		col_set_str(pinfo->cinfo, COL_PROTOCOL, "Teredo");
	if (check_col(pinfo->cinfo, COL_INFO))
		col_clear(pinfo->cinfo, COL_INFO);

	if (tree) {
		ti = proto_tree_add_item(tree, proto_teredo, tvb, 0, -1, FALSE);
		teredo_tree = proto_item_add_subtree(ti, ett_teredo);
	}
	else
		teredo_tree = NULL;

	teredoh->th_header  = tvb_get_ntohs(tvb, offset);

	if (teredoh->th_header == 1) {
		offset = parse_teredo_auth(tvb, pinfo, teredo_tree,
						offset, teredoh);
		teredoh->th_header  = tvb_get_ntohs(tvb, offset);
	}
	else
		teredoh->th_indtyp  = 0;

	if ( teredoh->th_header == 0 ) {
		offset = parse_teredo_orig(tvb, pinfo, teredo_tree,
						offset, teredoh);
	}

	teredoh->th_ip_v_hl = tvb_get_guint8(tvb, offset);

	decode_teredo_ports(tvb, offset, pinfo, tree, teredoh->th_header /* , teredoh->th_orgport*/);
	tap_queue_packet(teredo_tap, pinfo, teredoh);    
}

void
proto_register_teredo(void)
{
	static hf_register_info hf[] = {
		/* Authentication header */
		{ &hf_teredo_auth,
		{ "Teredo Authentication header", "teredo.auth",
		  FT_NONE, BASE_NONE, NULL, 0x0,
		  "Teredo Authentication header", HFILL }},
  
		{ &hf_teredo_auth_idlen,
		{ "Client identifier length", "teredo.auth.idlen",
		  FT_UINT8, BASE_DEC, NULL, 0x0,
		  "Client identifier length (ID-len)", HFILL }},

		{ &hf_teredo_auth_aulen,
		{ "Authentication value length", "teredo.auth.aulen",
		  FT_UINT8, BASE_DEC, NULL, 0x0,
		  "Authentication value length (AU-len)", HFILL }},

		{ &hf_teredo_auth_id,
		{ "Client identifier", "teredo.auth.id",
		  FT_BYTES, BASE_NONE, NULL, 0x0,
		  "Client identifier (ID)", HFILL }},

		{ &hf_teredo_auth_value,
		{ "Authentication value", "teredo.auth.value",
		  FT_BYTES, BASE_NONE, NULL, 0x0,
		  "Authentication value (hash)", HFILL }},

		{ &hf_teredo_auth_nonce,
		{ "Nonce value", "teredo.auth.nonce",
		  FT_BYTES, BASE_NONE, NULL, 0x0,
		  "Nonce value prevents spoofing Teredo server.",
		  HFILL }},

		{ &hf_teredo_auth_conf,
		{ "Confirmation byte", "teredo.auth.conf",
		  FT_BYTES, BASE_NONE, NULL, 0x0,
		  "Confirmation byte is zero upon successful authentication.",
		  HFILL }},

		/* Origin indication */
		{ &hf_teredo_orig,
		{ "Teredo Origin Indication header", "teredo.orig",
		  FT_NONE, BASE_NONE, NULL, 0x0,
		  "Teredo Origin Indication", HFILL }},

		{ &hf_teredo_orig_port,
		{ "Origin UDP port", "teredo.orig.port",
		  FT_UINT16, BASE_DEC, NULL, 0x0,
		  "Origin UDP port", HFILL }},

		{ &hf_teredo_orig_addr,
		{ "Origin IPv4 address", "teredo.orig.addr",
		  FT_IPv4, BASE_NONE, NULL, 0x0,
		  "Origin IPv4 address", HFILL }},
	};

	static gint *ett[] = {
		&ett_teredo, &ett_teredo_auth, &ett_teredo_orig
	};

	proto_teredo = proto_register_protocol(
		"Teredo IPv6 over UDP tunneling", "Teredo", "teredo");
	proto_register_field_array(proto_teredo, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));

/* subdissector code */
	teredo_dissector_table = register_dissector_table("teredo","Teredo ", FT_UINT16, BASE_DEC);
/*	register_heur_dissector_list("teredo.heur", &heur_subdissector_list); */

}

void
proto_reg_handoff_teredo(void)
{
	dissector_handle_t teredo_handle;

	teredo_handle = create_dissector_handle(dissect_teredo, proto_teredo);
	data_handle   = find_dissector("ipv6");
	teredo_tap    = register_tap("teredo");

	dissector_add("udp.port", UDP_PORT_TEREDO, teredo_handle);
}

