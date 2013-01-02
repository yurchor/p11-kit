/*
 * Copyright (C) 2012 Red Hat Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "attrs.h"
#include "checksum.h"
#define P11_DEBUG_FLAG P11_DEBUG_TRUST
#include "debug.h"
#include "dict.h"
#include "library.h"
#include "mozilla.h"
#include "oid.h"
#include "parser.h"

#include "pkcs11.h"
#include "pkcs11x.h"

#include <stdlib.h>
#include <string.h>

static CK_ATTRIBUTE *
update_ku (p11_parser *parser,
           p11_array *parsing,
           CK_ATTRIBUTE *object)
{
	unsigned char *data;
	unsigned int ku = 0;
	size_t length;
	CK_TRUST present;
	CK_TRUST defawlt;
	CK_ULONG i;

	struct {
		CK_ATTRIBUTE_TYPE type;
		unsigned int ku;
	} ku_attribute_map[] = {
		{ CKA_TRUST_DIGITAL_SIGNATURE, P11_KU_DIGITAL_SIGNATURE },
		{ CKA_TRUST_NON_REPUDIATION, P11_KU_NON_REPUDIATION },
		{ CKA_TRUST_KEY_ENCIPHERMENT, P11_KU_KEY_ENCIPHERMENT },
		{ CKA_TRUST_DATA_ENCIPHERMENT, P11_KU_DATA_ENCIPHERMENT },
		{ CKA_TRUST_KEY_AGREEMENT, P11_KU_KEY_AGREEMENT },
		{ CKA_TRUST_KEY_CERT_SIGN, P11_KU_KEY_CERT_SIGN },
		{ CKA_TRUST_CRL_SIGN, P11_KU_CRL_SIGN },
		{ CKA_INVALID },
	};

	CK_ATTRIBUTE attrs[sizeof (ku_attribute_map)];

	if (p11_parsing_get_flags (parser) & P11_PARSE_FLAG_ANCHOR)
		present = CKT_NETSCAPE_TRUSTED_DELEGATOR;
	else
		present = CKT_NETSCAPE_TRUSTED;
	defawlt = present;

	data = p11_parsing_get_extension (parser, parsing, P11_OID_KEY_USAGE, &length);

	/*
	 * If the certificate extension was missing, then *all* key
	 * usages are to be set. If the extension was invalid, then
	 * fail safe to none of the key usages.
	 */

	if (data) {
		defawlt = CKT_NETSCAPE_TRUST_UNKNOWN;
		if (p11_parse_key_usage (parser, data, length, &ku) != P11_PARSE_SUCCESS)
			p11_message ("invalid key usage certificate extension");
		free (data);
	}

	for (i = 0; ku_attribute_map[i].type != CKA_INVALID; i++) {
		attrs[i].type = ku_attribute_map[i].type;
		if (data && (ku & ku_attribute_map[i].ku) == ku_attribute_map[i].ku) {
			attrs[i].pValue = &present;
			attrs[i].ulValueLen = sizeof (present);
		} else {
			attrs[i].pValue = &defawlt;
			attrs[i].ulValueLen = sizeof (defawlt);
		}
	}

	return p11_attrs_buildn (object, attrs, i);
}

static CK_ATTRIBUTE *
update_eku (p11_parser *parser,
            p11_array *parsing,
            CK_ATTRIBUTE *object)
{
	CK_TRUST trust;
	CK_TRUST defawlt;
	CK_TRUST distrust;
	unsigned char *data;
	p11_dict *ekus = NULL;
	p11_dict *reject = NULL;
	size_t length;
	CK_ULONG i;

	struct {
		CK_ATTRIBUTE_TYPE type;
		const unsigned char *eku;
	} eku_attribute_map[] = {
		{ CKA_TRUST_SERVER_AUTH, P11_OID_SERVER_AUTH },
		{ CKA_TRUST_CLIENT_AUTH, P11_OID_CLIENT_AUTH },
		{ CKA_TRUST_CODE_SIGNING, P11_OID_CODE_SIGNING },
		{ CKA_TRUST_EMAIL_PROTECTION, P11_OID_EMAIL_PROTECTION },
		{ CKA_TRUST_IPSEC_END_SYSTEM, P11_OID_IPSEC_END_SYSTEM },
		{ CKA_TRUST_IPSEC_TUNNEL, P11_OID_IPSEC_TUNNEL },
		{ CKA_TRUST_IPSEC_USER, P11_OID_IPSEC_USER },
		{ CKA_TRUST_TIME_STAMPING, P11_OID_TIME_STAMPING },
		{ CKA_INVALID },
	};


	CK_ATTRIBUTE attrs[sizeof (eku_attribute_map)];

	/* The value set if an eku is present */
	if (p11_parsing_get_flags (parser) & P11_PARSE_FLAG_ANCHOR)
		trust = CKT_NETSCAPE_TRUSTED_DELEGATOR;
	else
		trust= CKT_NETSCAPE_TRUSTED;

	/* The value set if an eku is not present, adjusted below */
	defawlt = trust;

	/* The value set if an eku is explictly rejected */
	distrust = CKT_NETSCAPE_UNTRUSTED;

	/*
	 * If the certificate extension was missing, then *all* extended key
	 * usages are to be set. If the extension was invalid, then
	 * fail safe to none of the extended key usages.
	 */

	data = p11_parsing_get_extension (parser, parsing, P11_OID_EXTENDED_KEY_USAGE, &length);

	if (data) {
		defawlt = CKT_NETSCAPE_TRUST_UNKNOWN;
		ekus = p11_parse_extended_key_usage (parser, data, length);
		if (ekus == NULL)
			p11_message ("invalid extended key usage certificate extension");
		free (data);
	}

	data = p11_parsing_get_extension (parser, parsing, P11_OID_OPENSSL_REJECT, &length);
	if (data) {
		reject = p11_parse_extended_key_usage (parser, data, length);
		if (reject == NULL)
			p11_message ("invalid reject key usage certificate extension");
		free (data);
	}

	for (i = 0; eku_attribute_map[i].type != CKA_INVALID; i++) {
		attrs[i].type = eku_attribute_map[i].type;
		if (reject && p11_dict_get (reject, eku_attribute_map[i].eku)) {
			attrs[i].pValue = &distrust;
			attrs[i].ulValueLen = sizeof (distrust);
		} else if (ekus && p11_dict_get (ekus, eku_attribute_map[i].eku)) {
			attrs[i].pValue = &trust;
			attrs[i].ulValueLen = sizeof (trust);
		} else {
			attrs[i].pValue = &defawlt;
			attrs[i].ulValueLen = sizeof (defawlt);
		}
	}

	p11_dict_free (ekus);
	p11_dict_free (reject);

	return p11_attrs_buildn (object, attrs, i);
}


static CK_ATTRIBUTE *
build_nss_trust_object (p11_parser *parser,
                        p11_array *parsing,
                        CK_ATTRIBUTE *cert)
{
	CK_ATTRIBUTE *object = NULL;

	CK_OBJECT_CLASS vclass = CKO_NETSCAPE_TRUST;
	CK_BYTE vsha1_hash[P11_CHECKSUM_SHA1_LENGTH];
	CK_BYTE vmd5_hash[P11_CHECKSUM_MD5_LENGTH];
	CK_BBOOL vfalse = CK_FALSE;
	CK_BBOOL vtrue = CK_TRUE;

	CK_ATTRIBUTE klass = { CKA_CLASS, &vclass, sizeof (vclass) };
	CK_ATTRIBUTE token = { CKA_TOKEN, &vtrue, sizeof (vtrue) };
	CK_ATTRIBUTE private = { CKA_PRIVATE, &vfalse, sizeof (vfalse) };
	CK_ATTRIBUTE modifiable = { CKA_MODIFIABLE, &vfalse, sizeof (vfalse) };
	CK_ATTRIBUTE invalid = { CKA_INVALID, };

	CK_ATTRIBUTE md5_hash = { CKA_CERT_MD5_HASH, vmd5_hash, sizeof (vmd5_hash) };
	CK_ATTRIBUTE sha1_hash = { CKA_CERT_SHA1_HASH, vsha1_hash, sizeof (vsha1_hash) };

	CK_ATTRIBUTE step_up_approved = { CKA_TRUST_STEP_UP_APPROVED, &vfalse, sizeof (vfalse) };

	CK_ATTRIBUTE *label;
	CK_ATTRIBUTE *id;
	CK_ATTRIBUTE *der;
	CK_ATTRIBUTE *subject;
	CK_ATTRIBUTE *issuer;
	CK_ATTRIBUTE *serial_number;

	/* Setup the hashes of the DER certificate value */
	der = p11_attrs_find (cert, CKA_VALUE);
	return_val_if_fail (der != NULL, NULL);
	p11_checksum_md5 (vmd5_hash, der->pValue, der->ulValueLen, NULL);
	p11_checksum_sha1 (vsha1_hash, der->pValue, der->ulValueLen, NULL);

	/* Copy all of the following attributes from certificate */
	id = p11_attrs_find (cert, CKA_ID);
	return_val_if_fail (id != NULL, NULL);
	subject = p11_attrs_find (cert, CKA_SUBJECT);
	return_val_if_fail (subject != NULL, NULL);
	issuer = p11_attrs_find (cert, CKA_ISSUER);
	return_val_if_fail (issuer != NULL, NULL);
	serial_number = p11_attrs_find (cert, CKA_SERIAL_NUMBER);
	return_val_if_fail (serial_number != NULL, NULL);

	/* Try to use the same label */
	label = p11_attrs_find (cert, CKA_LABEL);
	if (label == NULL)
		label = &invalid;

	object = p11_attrs_build (NULL, &klass, &token, &private, &modifiable, id, label,
	                          subject, issuer, serial_number, &md5_hash, &sha1_hash,
	                          &step_up_approved, NULL);
	return_val_if_fail (object != NULL, NULL);

	object = update_ku (parser, parsing, object);
	return_val_if_fail (object != NULL, NULL);

	object = update_eku (parser, parsing, object);
	return_val_if_fail (object != NULL, NULL);

	if (!p11_array_push (parsing, object))
		return_val_if_reached (NULL);

	return object;
}

void
p11_mozilla_build_trust_object (p11_parser *parser,
                                p11_array *parsing)
{
	CK_ATTRIBUTE *cert;

	cert = p11_parsing_get_certificate (parser, parsing);
	return_if_fail (cert != NULL);

	build_nss_trust_object (parser, parsing, cert);
}