/* notmuch - Not much of an email program, (just index and search)
 *
 * Copyright © 2009 Carl Worth
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see https://www.gnu.org/licenses/ .
 *
 * Author: Carl Worth <cworth@cworth.org>
 */

#include "notmuch-client.h"
#include "gmime-filter-reply.h"
#include "sprinter.h"

static const char *
_get_tags_as_string (const void *ctx, notmuch_message_t *message)
{
    notmuch_tags_t *tags;
    int first = 1;
    const char *tag;
    char *result;

    result = talloc_strdup (ctx, "");
    if (result == NULL)
	return NULL;

    for (tags = notmuch_message_get_tags (message);
	 notmuch_tags_valid (tags);
	 notmuch_tags_move_to_next (tags))
    {
	tag = notmuch_tags_get (tags);

	result = talloc_asprintf_append (result, "%s%s",
					 first ? "" : " ", tag);
	first = 0;
    }

    return result;
}

/* Get a nice, single-line summary of message. */
static const char *
_get_one_line_summary (const void *ctx, notmuch_message_t *message)
{
    const char *from;
    time_t date;
    const char *relative_date;
    const char *tags;

    from = notmuch_message_get_header (message, "from");

    date = notmuch_message_get_date (message);
    relative_date = notmuch_time_relative_date (ctx, date);

    tags = _get_tags_as_string (ctx, message);

    return talloc_asprintf (ctx, "%s (%s) (%s)",
			    from, relative_date, tags);
}

static const char *_get_disposition(GMimeObject *meta)
{
    GMimeContentDisposition *disposition;

    disposition = g_mime_object_get_content_disposition (meta);
    if (!disposition)
	return NULL;

    return g_mime_content_disposition_get_disposition (disposition);
}

/* Emit a sequence of key/value pairs for the metadata of message.
 * The caller should begin a map before calling this. */
static void
format_message_sprinter (sprinter_t *sp, notmuch_message_t *message)
{
    /* Any changes to the JSON or S-Expression format should be
     * reflected in the file devel/schemata. */

    void *local = talloc_new (NULL);
    notmuch_tags_t *tags;
    time_t date;
    const char *relative_date;

    sp->map_key (sp, "id");
    sp->string (sp, notmuch_message_get_message_id (message));

    sp->map_key (sp, "match");
    sp->boolean (sp, notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_MATCH));

    sp->map_key (sp, "excluded");
    sp->boolean (sp, notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_EXCLUDED));

    sp->map_key (sp, "filename");
    if (notmuch_format_version >= 3) {
	notmuch_filenames_t *filenames;

	sp->begin_list (sp);
	for (filenames = notmuch_message_get_filenames (message);
	     notmuch_filenames_valid (filenames);
	     notmuch_filenames_move_to_next (filenames)) {
	    sp->string (sp, notmuch_filenames_get (filenames));
	}
	notmuch_filenames_destroy (filenames);
	sp->end (sp);
    } else {
	sp->string (sp, notmuch_message_get_filename (message));
    }

    sp->map_key (sp, "timestamp");
    date = notmuch_message_get_date (message);
    sp->integer (sp, date);

    sp->map_key (sp, "date_relative");
    relative_date = notmuch_time_relative_date (local, date);
    sp->string (sp, relative_date);

    sp->map_key (sp, "tags");
    sp->begin_list (sp);
    for (tags = notmuch_message_get_tags (message);
	 notmuch_tags_valid (tags);
	 notmuch_tags_move_to_next (tags))
	sp->string (sp, notmuch_tags_get (tags));
    sp->end (sp);

    talloc_free (local);
}

/* Extract just the email address from the contents of a From:
 * header. */
static const char *
_extract_email_address (const void *ctx, const char *from)
{
    InternetAddressList *addresses;
    InternetAddress *address;
    InternetAddressMailbox *mailbox;
    const char *email = "MAILER-DAEMON";

    addresses = internet_address_list_parse_string (from);

    /* Bail if there is no address here. */
    if (addresses == NULL || internet_address_list_length (addresses) < 1)
	goto DONE;

    /* Otherwise, just use the first address. */
    address = internet_address_list_get_address (addresses, 0);

    /* The From header should never contain an address group rather
     * than a mailbox. So bail if it does. */
    if (! INTERNET_ADDRESS_IS_MAILBOX (address))
	goto DONE;

    mailbox = INTERNET_ADDRESS_MAILBOX (address);
    email = internet_address_mailbox_get_addr (mailbox);
    email = talloc_strdup (ctx, email);

  DONE:
    if (addresses)
	g_object_unref (addresses);

    return email;
   }

/* Return 1 if 'line' is an mbox From_ line---that is, a line
 * beginning with zero or more '>' characters followed by the
 * characters 'F', 'r', 'o', 'm', and space.
 *
 * Any characters at all may appear after that in the line.
 */
static int
_is_from_line (const char *line)
{
    const char *s = line;

    if (line == NULL)
	return 0;

    while (*s == '>')
	s++;

    if (STRNCMP_LITERAL (s, "From ") == 0)
	return 1;
    else
	return 0;
}

void
format_headers_sprinter (sprinter_t *sp, GMimeMessage *message,
			 bool reply)
{
    /* Any changes to the JSON or S-Expression format should be
     * reflected in the file devel/schemata. */

    char *recipients_string;
    const char *reply_to_string;
    void *local = talloc_new (sp);

    sp->begin_map (sp);

    sp->map_key (sp, "Subject");
    sp->string (sp, g_mime_message_get_subject (message));

    sp->map_key (sp, "From");
    sp->string (sp, g_mime_message_get_from_string (message));

    recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_TO);
    if (recipients_string) {
	sp->map_key (sp, "To");
	sp->string (sp, recipients_string);
	g_free (recipients_string);
    }

    recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_CC);
    if (recipients_string) {
	sp->map_key (sp, "Cc");
	sp->string (sp, recipients_string);
	g_free (recipients_string);
    }

    recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_BCC);
    if (recipients_string) {
	sp->map_key (sp, "Bcc");
	sp->string (sp, recipients_string);
	g_free (recipients_string);
    }

    reply_to_string = g_mime_message_get_reply_to_string (local, message);
    if (reply_to_string) {
	sp->map_key (sp, "Reply-To");
	sp->string (sp, reply_to_string);
    }

    if (reply) {
	sp->map_key (sp, "In-reply-to");
	sp->string (sp, g_mime_object_get_header (GMIME_OBJECT (message), "In-reply-to"));

	sp->map_key (sp, "References");
	sp->string (sp, g_mime_object_get_header (GMIME_OBJECT (message), "References"));
    } else {
	sp->map_key (sp, "Date");
	sp->string (sp, g_mime_message_get_date_string (sp, message));
    }

    sp->end (sp);
    talloc_free (local);
}

/* Write a MIME text part out to the given stream.
 *
 * If (flags & NOTMUCH_SHOW_TEXT_PART_REPLY), this prepends "> " to
 * each output line.
 *
 * Both line-ending conversion (CRLF->LF) and charset conversion ( ->
 * UTF-8) will be performed, so it is inappropriate to call this
 * function with a non-text part. Doing so will trigger an internal
 * error.
 */
void
show_text_part_content (GMimeObject *part, GMimeStream *stream_out,
			notmuch_show_text_part_flags flags)
{
    GMimeContentType *content_type = g_mime_object_get_content_type (GMIME_OBJECT (part));
    GMimeStream *stream_filter = NULL;
    GMimeFilter *crlf_filter = NULL;
    GMimeDataWrapper *wrapper;
    const char *charset;

    if (! g_mime_content_type_is_type (content_type, "text", "*"))
	INTERNAL_ERROR ("Illegal request to format non-text part (%s) as text.",
			g_mime_content_type_to_string (content_type));

    if (stream_out == NULL)
	return;

    stream_filter = g_mime_stream_filter_new (stream_out);
    crlf_filter = g_mime_filter_crlf_new (false, false);
    g_mime_stream_filter_add(GMIME_STREAM_FILTER (stream_filter),
			     crlf_filter);
    g_object_unref (crlf_filter);

    charset = g_mime_object_get_content_type_parameter (part, "charset");
    if (charset) {
	GMimeFilter *charset_filter;
	charset_filter = g_mime_filter_charset_new (charset, "UTF-8");
	/* This result can be NULL for things like "unknown-8bit".
	 * Don't set a NULL filter as that makes GMime print
	 * annoying assertion-failure messages on stderr. */
	if (charset_filter) {
	    g_mime_stream_filter_add (GMIME_STREAM_FILTER (stream_filter),
				      charset_filter);
	    g_object_unref (charset_filter);
	}

    }

    if (flags & NOTMUCH_SHOW_TEXT_PART_REPLY) {
	GMimeFilter *reply_filter;
	reply_filter = g_mime_filter_reply_new (true);
	if (reply_filter) {
	    g_mime_stream_filter_add (GMIME_STREAM_FILTER (stream_filter),
				      reply_filter);
	    g_object_unref (reply_filter);
	}
    }

    wrapper = g_mime_part_get_content_object (GMIME_PART (part));
    if (wrapper && stream_filter)
	g_mime_data_wrapper_write_to_stream (wrapper, stream_filter);
    if (stream_filter)
	g_object_unref(stream_filter);
}

static const char*
signature_status_to_string (GMimeSignatureStatus status)
{
    if (g_mime_signature_status_bad (status))
	return "bad";

    if (g_mime_signature_status_error (status))
	return "error";

    if (g_mime_signature_status_good (status))
	return "good";

    return "unknown";
}

/* Print signature flags */
struct key_map_struct {
    GMimeSignatureError bit;
    const char * string;
};

static void
do_format_signature_errors (sprinter_t *sp, struct key_map_struct *key_map,
			    unsigned int array_map_len, GMimeSignatureError errors) {
    sp->map_key (sp, "errors");
    sp->begin_map (sp);

    for (unsigned int i = 0; i < array_map_len; i++) {
	if (errors & key_map[i].bit) {
	    sp->map_key (sp, key_map[i].string);
	    sp->boolean (sp, true);
	}
    }

    sp->end (sp);
}

#if (GMIME_MAJOR_VERSION < 3)
static void
format_signature_errors (sprinter_t *sp, GMimeSignature *signature)
{
    GMimeSignatureError errors = g_mime_signature_get_errors (signature);

    if (errors == GMIME_SIGNATURE_ERROR_NONE)
	return;

    struct key_map_struct key_map[] = {
	{ GMIME_SIGNATURE_ERROR_EXPSIG, "sig-expired" },
	{ GMIME_SIGNATURE_ERROR_NO_PUBKEY, "key-missing"},
	{ GMIME_SIGNATURE_ERROR_EXPKEYSIG, "key-expired"},
	{ GMIME_SIGNATURE_ERROR_REVKEYSIG, "key-revoked"},
	{ GMIME_SIGNATURE_ERROR_UNSUPP_ALGO, "alg-unsupported"},
    };

    do_format_signature_errors (sp, key_map, ARRAY_SIZE(key_map), errors);
}
#else
static void
format_signature_errors (sprinter_t *sp, GMimeSignature *signature)
{
    GMimeSignatureError errors = g_mime_signature_get_errors (signature);

    if (!(errors & GMIME_SIGNATURE_STATUS_ERROR_MASK))
	return;

    struct key_map_struct key_map[] = {
	{ GMIME_SIGNATURE_STATUS_KEY_REVOKED, "key-revoked"},
	{ GMIME_SIGNATURE_STATUS_KEY_EXPIRED, "key-expired"},
	{ GMIME_SIGNATURE_STATUS_SIG_EXPIRED, "sig-expired" },
	{ GMIME_SIGNATURE_STATUS_KEY_MISSING, "key-missing"},
	{ GMIME_SIGNATURE_STATUS_CRL_MISSING, "crl-missing"},
	{ GMIME_SIGNATURE_STATUS_CRL_TOO_OLD, "crl-too-old"},
	{ GMIME_SIGNATURE_STATUS_BAD_POLICY, "bad-policy"},
	{ GMIME_SIGNATURE_STATUS_SYS_ERROR, "sys-error"},
	{ GMIME_SIGNATURE_STATUS_TOFU_CONFLICT, "tofu-conflict"},
    };

    do_format_signature_errors (sp, key_map, ARRAY_SIZE(key_map), errors);
}
#endif

/* Signature status sprinter (GMime 2.6) */
static void
format_part_sigstatus_sprinter (sprinter_t *sp, mime_node_t *node)
{
    /* Any changes to the JSON or S-Expression format should be
     * reflected in the file devel/schemata. */

    GMimeSignatureList *siglist = node->sig_list;

    sp->begin_list (sp);

    if (!siglist) {
	sp->end (sp);
	return;
    }

    int i;
    for (i = 0; i < g_mime_signature_list_length (siglist); i++) {
	GMimeSignature *signature = g_mime_signature_list_get_signature (siglist, i);

	sp->begin_map (sp);

	/* status */
	GMimeSignatureStatus status = g_mime_signature_get_status (signature);
	sp->map_key (sp, "status");
	sp->string (sp, signature_status_to_string (status));

	GMimeCertificate *certificate = g_mime_signature_get_certificate (signature);
	if (g_mime_signature_status_good (status)) {
	    if (certificate) {
		sp->map_key (sp, "fingerprint");
		sp->string (sp, g_mime_certificate_get_fingerprint (certificate));
	    }
	    /* these dates are seconds since the epoch; should we
	     * provide a more human-readable format string? */
	    time_t created = g_mime_signature_get_created (signature);
	    if (created != -1) {
		sp->map_key (sp, "created");
		sp->integer (sp, created);
	    }
	    time_t expires = g_mime_signature_get_expires (signature);
	    if (expires > 0) {
		sp->map_key (sp, "expires");
		sp->integer (sp, expires);
	    }
	    if (certificate) {
		const char *uid = g_mime_certificate_get_valid_userid (certificate);
		if (uid) {
		    sp->map_key (sp, "userid");
		    sp->string (sp, uid);
		}
	    }
	} else if (certificate) {
	    const char *key_id = g_mime_certificate_get_fpr16 (certificate);
	    if (key_id) {
		sp->map_key (sp, "keyid");
		sp->string (sp, key_id);
	    }
	}

	if (notmuch_format_version <= 3) {
	    GMimeSignatureError errors = g_mime_signature_get_errors (signature);
	    if (g_mime_signature_status_error (errors)) {
		sp->map_key (sp, "errors");
		sp->integer (sp, errors);
	    }
	} else {
	    format_signature_errors (sp, signature);
	}

	sp->end (sp);
     }

    sp->end (sp);
}

static notmuch_status_t
format_part_text (const void *ctx, sprinter_t *sp, mime_node_t *node,
		  int indent, const notmuch_show_params_t *params)
{
    /* The disposition and content-type metadata are associated with
     * the envelope for message parts */
    GMimeObject *meta = node->envelope_part ?
	GMIME_OBJECT (node->envelope_part) : node->part;
    GMimeContentType *content_type = g_mime_object_get_content_type (meta);
    const bool leaf = GMIME_IS_PART (node->part);
    GMimeStream *stream = params->out_stream;
    const char *part_type;
    int i;

    if (node->envelope_file) {
	notmuch_message_t *message = node->envelope_file;

	part_type = "message";
	g_mime_stream_printf (stream, "\f%s{ id:%s depth:%d match:%d excluded:%d filename:%s\n",
			      part_type,
			      notmuch_message_get_message_id (message),
			      indent,
			      notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_MATCH) ? 1 : 0,
			      notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_EXCLUDED) ? 1 : 0,
			      notmuch_message_get_filename (message));
    } else {
	char *content_string;
	const char *disposition = _get_disposition (meta);
	const char *cid = g_mime_object_get_content_id (meta);
	const char *filename = leaf ?
	    g_mime_part_get_filename (GMIME_PART (node->part)) : NULL;

	if (disposition &&
	    strcasecmp (disposition, GMIME_DISPOSITION_ATTACHMENT) == 0)
	    part_type = "attachment";
	else
	    part_type = "part";

	g_mime_stream_printf (stream, "\f%s{ ID: %d", part_type, node->part_num);
	if (filename)
	    g_mime_stream_printf (stream, ", Filename: %s", filename);
	if (cid)
	    g_mime_stream_printf (stream, ", Content-id: %s", cid);

	content_string = g_mime_content_type_to_string (content_type);
	g_mime_stream_printf (stream, ", Content-type: %s\n", content_string);
	g_free (content_string);
    }

    if (GMIME_IS_MESSAGE (node->part)) {
	GMimeMessage *message = GMIME_MESSAGE (node->part);
	char *recipients_string;
	char *date_string;

	g_mime_stream_printf (stream, "\fheader{\n");
	if (node->envelope_file)
	    g_mime_stream_printf (stream, "%s\n", _get_one_line_summary (ctx, node->envelope_file));
	g_mime_stream_printf (stream, "Subject: %s\n", g_mime_message_get_subject (message));
	g_mime_stream_printf (stream, "From: %s\n", g_mime_message_get_from_string (message));
	recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_TO);
	if (recipients_string)
	    g_mime_stream_printf (stream, "To: %s\n", recipients_string);
	g_free (recipients_string);
	recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_CC);
	if (recipients_string)
	    g_mime_stream_printf (stream, "Cc: %s\n", recipients_string);
	g_free (recipients_string);
	date_string = g_mime_message_get_date_string (node, message);
	g_mime_stream_printf (stream, "Date: %s\n", date_string);
	g_mime_stream_printf (stream, "\fheader}\n");

	g_mime_stream_printf (stream, "\fbody{\n");
    }

    if (leaf) {
	if (g_mime_content_type_is_type (content_type, "text", "*") &&
	    !g_mime_content_type_is_type (content_type, "text", "html"))
	{
	    show_text_part_content (node->part, stream, 0);
	} else {
	    char *content_string = g_mime_content_type_to_string (content_type);
	    g_mime_stream_printf (stream, "Non-text part: %s\n", content_string);
	    g_free (content_string);
	}
    }

    for (i = 0; i < node->nchildren; i++)
	format_part_text (ctx, sp, mime_node_child (node, i), indent, params);

    if (GMIME_IS_MESSAGE (node->part))
	g_mime_stream_printf (stream, "\fbody}\n");

    g_mime_stream_printf (stream, "\f%s}\n", part_type);

    return NOTMUCH_STATUS_SUCCESS;
}

static void
format_omitted_part_meta_sprinter (sprinter_t *sp, GMimeObject *meta, GMimePart *part)
{
    const char *content_charset = g_mime_object_get_content_type_parameter (meta, "charset");
    const char *cte = g_mime_object_get_header (meta, "content-transfer-encoding");
    GMimeDataWrapper *wrapper = g_mime_part_get_content_object (part);
    GMimeStream *stream = g_mime_data_wrapper_get_stream (wrapper);
    ssize_t content_length = g_mime_stream_length (stream);

    if (content_charset != NULL) {
	sp->map_key (sp, "content-charset");
	sp->string (sp, content_charset);
    }
    if (cte != NULL) {
	sp->map_key (sp, "content-transfer-encoding");
	sp->string (sp, cte);
    }
    if (content_length >= 0) {
	sp->map_key (sp, "content-length");
	sp->integer (sp, content_length);
    }
}

void
format_part_sprinter (const void *ctx, sprinter_t *sp, mime_node_t *node,
		      bool output_body,
		      bool include_html)
{
    /* Any changes to the JSON or S-Expression format should be
     * reflected in the file devel/schemata. */

    if (node->envelope_file) {
	sp->begin_map (sp);
	format_message_sprinter (sp, node->envelope_file);

	sp->map_key (sp, "headers");
	format_headers_sprinter (sp, GMIME_MESSAGE (node->part), false);

	if (output_body) {
	    sp->map_key (sp, "body");
	    sp->begin_list (sp);
	    format_part_sprinter (ctx, sp, mime_node_child (node, 0), true, include_html);
	    sp->end (sp);
	}
	sp->end (sp);
	return;
    }

    /* The disposition and content-type metadata are associated with
     * the envelope for message parts */
    GMimeObject *meta = node->envelope_part ?
	GMIME_OBJECT (node->envelope_part) : node->part;
    GMimeContentType *content_type = g_mime_object_get_content_type (meta);
    char *content_string;
    const char *disposition = _get_disposition (meta);
    const char *cid = g_mime_object_get_content_id (meta);
    const char *filename = GMIME_IS_PART (node->part) ?
	g_mime_part_get_filename (GMIME_PART (node->part)) : NULL;
    int nclose = 0;
    int i;

    sp->begin_map (sp);

    sp->map_key (sp, "id");
    sp->integer (sp, node->part_num);

    if (node->decrypt_attempted) {
	sp->map_key (sp, "encstatus");
	sp->begin_list (sp);
	sp->begin_map (sp);
	sp->map_key (sp, "status");
	sp->string (sp, node->decrypt_success ? "good" : "bad");
	sp->end (sp);
	sp->end (sp);
    }

    if (node->verify_attempted) {
	sp->map_key (sp, "sigstatus");
	format_part_sigstatus_sprinter (sp, node);
    }

    sp->map_key (sp, "content-type");
    content_string = g_mime_content_type_to_string (content_type);
    sp->string (sp, content_string);
    g_free (content_string);

    if (disposition) {
	sp->map_key (sp, "content-disposition");
	sp->string (sp, disposition);
    }

    if (cid) {
	sp->map_key (sp, "content-id");
	sp->string (sp, cid);
    }

    if (filename) {
	sp->map_key (sp, "filename");
	sp->string (sp, filename);
    }

    if (GMIME_IS_PART (node->part)) {
	/* For non-HTML text parts, we include the content in the
	 * JSON. Since JSON must be Unicode, we handle charset
	 * decoding here and do not report a charset to the caller.
	 * For text/html parts, we do not include the content unless
	 * the --include-html option has been passed. If a html part
	 * is not included, it can be requested directly. This makes
	 * charset decoding the responsibility on the caller so we
	 * report the charset for text/html parts.
	 */
	if (g_mime_content_type_is_type (content_type, "text", "*") &&
	    (include_html ||
	     ! g_mime_content_type_is_type (content_type, "text", "html")))
	{
	    GMimeStream *stream_memory = g_mime_stream_mem_new ();
	    GByteArray *part_content;
	    show_text_part_content (node->part, stream_memory, 0);
	    part_content = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (stream_memory));
	    sp->map_key (sp, "content");
	    sp->string_len (sp, (char *) part_content->data, part_content->len);
	    g_object_unref (stream_memory);
	} else {
	    format_omitted_part_meta_sprinter (sp, meta, GMIME_PART (node->part));
	}
    } else if (GMIME_IS_MULTIPART (node->part)) {
	sp->map_key (sp, "content");
	sp->begin_list (sp);
	nclose = 1;
    } else if (GMIME_IS_MESSAGE (node->part)) {
	sp->map_key (sp, "content");
	sp->begin_list (sp);
	sp->begin_map (sp);

	sp->map_key (sp, "headers");
	format_headers_sprinter (sp, GMIME_MESSAGE (node->part), false);

	sp->map_key (sp, "body");
	sp->begin_list (sp);
	nclose = 3;
    }

    for (i = 0; i < node->nchildren; i++)
	format_part_sprinter (ctx, sp, mime_node_child (node, i), true, include_html);

    /* Close content structures */
    for (i = 0; i < nclose; i++)
	sp->end (sp);
    /* Close part map */
    sp->end (sp);
}

static notmuch_status_t
format_part_sprinter_entry (const void *ctx, sprinter_t *sp,
			    mime_node_t *node, unused (int indent),
			    const notmuch_show_params_t *params)
{
    format_part_sprinter (ctx, sp, node, params->output_body, params->include_html);

    return NOTMUCH_STATUS_SUCCESS;
}

/* Print a message in "mboxrd" format as documented, for example,
 * here:
 *
 * http://qmail.org/qmail-manual-html/man5/mbox.html
 */
static notmuch_status_t
format_part_mbox (const void *ctx, unused (sprinter_t *sp), mime_node_t *node,
		  unused (int indent),
		  unused (const notmuch_show_params_t *params))
{
    notmuch_message_t *message = node->envelope_file;

    const char *filename;
    FILE *file;
    const char *from;

    time_t date;
    struct tm date_gmtime;
    char date_asctime[26];

    char *line = NULL;
    size_t line_size;
    ssize_t line_len;

    if (!message)
	INTERNAL_ERROR ("format_part_mbox requires a root part");

    filename = notmuch_message_get_filename (message);
    file = fopen (filename, "r");
    if (file == NULL) {
	fprintf (stderr, "Failed to open %s: %s\n",
		 filename, strerror (errno));
	return NOTMUCH_STATUS_FILE_ERROR;
    }

    from = notmuch_message_get_header (message, "from");
    from = _extract_email_address (ctx, from);

    date = notmuch_message_get_date (message);
    gmtime_r (&date, &date_gmtime);
    asctime_r (&date_gmtime, date_asctime);

    printf ("From %s %s", from, date_asctime);

    while ((line_len = getline (&line, &line_size, file)) != -1 ) {
	if (_is_from_line (line))
	    putchar ('>');
	printf ("%s", line);
    }

    printf ("\n");

    fclose (file);

    return NOTMUCH_STATUS_SUCCESS;
}

static notmuch_status_t
format_part_raw (unused (const void *ctx), unused (sprinter_t *sp),
		 mime_node_t *node, unused (int indent),
		 const notmuch_show_params_t *params)
{
    if (node->envelope_file) {
	/* Special case the entire message to avoid MIME parsing. */
	const char *filename;
	FILE *file;
	size_t size;
	char buf[4096];

	filename = notmuch_message_get_filename (node->envelope_file);
	if (filename == NULL) {
	    fprintf (stderr, "Error: Cannot get message filename.\n");
	    return NOTMUCH_STATUS_FILE_ERROR;
	}

	file = fopen (filename, "r");
	if (file == NULL) {
	    fprintf (stderr, "Error: Cannot open file %s: %s\n", filename, strerror (errno));
	    return NOTMUCH_STATUS_FILE_ERROR;
	}

	while (!feof (file)) {
	    size = fread (buf, 1, sizeof (buf), file);
	    if (ferror (file)) {
		fprintf (stderr, "Error: Read failed from %s\n", filename);
		fclose (file);
		return NOTMUCH_STATUS_FILE_ERROR;
	    }

	    if (fwrite (buf, size, 1, stdout) != 1) {
		fprintf (stderr, "Error: Write failed\n");
		fclose (file);
		return NOTMUCH_STATUS_FILE_ERROR;
	    }
	}

	fclose (file);
	return NOTMUCH_STATUS_SUCCESS;
    }

    GMimeStream *stream_filter = g_mime_stream_filter_new (params->out_stream);

    if (GMIME_IS_PART (node->part)) {
	/* For leaf parts, we emit only the transfer-decoded
	 * body. */
	GMimeDataWrapper *wrapper;
	wrapper = g_mime_part_get_content_object (GMIME_PART (node->part));

	if (wrapper && stream_filter)
	    g_mime_data_wrapper_write_to_stream (wrapper, stream_filter);
    } else {
	/* Write out the whole part.  For message parts (the root
	 * part and embedded message parts), this will be the
	 * message including its headers (but not the
	 * encapsulating part's headers).  For multipart parts,
	 * this will include the headers. */
	if (stream_filter)
	    g_mime_object_write_to_stream (node->part, stream_filter);
    }

    if (stream_filter)
	g_object_unref (stream_filter);

    return NOTMUCH_STATUS_SUCCESS;
}

static notmuch_status_t
show_message (void *ctx,
	      const notmuch_show_format_t *format,
	      sprinter_t *sp,
	      notmuch_message_t *message,
	      int indent,
	      notmuch_show_params_t *params)
{
    void *local = talloc_new (ctx);
    mime_node_t *root, *part;
    notmuch_status_t status;

    status = mime_node_open (local, message, &(params->crypto), &root);
    if (status)
	goto DONE;
    part = mime_node_seek_dfs (root, (params->part < 0 ? 0 : params->part));
    if (part)
	status = format->part (local, sp, part, indent, params);
  DONE:
    talloc_free (local);
    return status;
}

static notmuch_status_t
show_messages (void *ctx,
	       const notmuch_show_format_t *format,
	       sprinter_t *sp,
	       notmuch_messages_t *messages,
	       int indent,
	       notmuch_show_params_t *params)
{
    notmuch_message_t *message;
    bool match;
    bool excluded;
    int next_indent;
    notmuch_status_t status, res = NOTMUCH_STATUS_SUCCESS;

    sp->begin_list (sp);

    for (;
	 notmuch_messages_valid (messages);
	 notmuch_messages_move_to_next (messages))
    {
	sp->begin_list (sp);

	message = notmuch_messages_get (messages);

	match = notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_MATCH);
	excluded = notmuch_message_get_flag (message, NOTMUCH_MESSAGE_FLAG_EXCLUDED);

	next_indent = indent;

	if ((match && (!excluded || !params->omit_excluded)) || params->entire_thread) {
	    status = show_message (ctx, format, sp, message, indent, params);
	    if (status && !res)
		res = status;
	    next_indent = indent + 1;
	} else {
	    sp->null (sp);
	}

	status = show_messages (ctx,
				format, sp,
				notmuch_message_get_replies (message),
				next_indent,
				params);
	if (status && !res)
	    res = status;

	notmuch_message_destroy (message);

	sp->end (sp);
    }

    sp->end (sp);

    return res;
}

/* Formatted output of single message */
static int
do_show_single (void *ctx,
		notmuch_query_t *query,
		const notmuch_show_format_t *format,
		sprinter_t *sp,
		notmuch_show_params_t *params)
{
    notmuch_messages_t *messages;
    notmuch_message_t *message;
    notmuch_status_t status;
    unsigned int count;

    status = notmuch_query_count_messages (query, &count);
    if (print_status_query ("notmuch show", query, status))
	return 1;

    if (count != 1) {
	fprintf (stderr, "Error: search term did not match precisely one message (matched %u messages).\n", count);
	return 1;
    }

    status = notmuch_query_search_messages (query, &messages);
    if (print_status_query ("notmuch show", query, status))
	return 1;

    message = notmuch_messages_get (messages);

    if (message == NULL) {
	fprintf (stderr, "Error: Cannot find matching message.\n");
	return 1;
    }

    notmuch_message_set_flag (message, NOTMUCH_MESSAGE_FLAG_MATCH, 1);

    return show_message (ctx, format, sp, message, 0, params)
	!= NOTMUCH_STATUS_SUCCESS;
}

/* Formatted output of threads */
static int
do_show (void *ctx,
	 notmuch_query_t *query,
	 const notmuch_show_format_t *format,
	 sprinter_t *sp,
	 notmuch_show_params_t *params)
{
    notmuch_threads_t *threads;
    notmuch_thread_t *thread;
    notmuch_messages_t *messages;
    notmuch_status_t status, res = NOTMUCH_STATUS_SUCCESS;

    status= notmuch_query_search_threads (query, &threads);
    if (print_status_query ("notmuch show", query, status))
	return 1;

    sp->begin_list (sp);

    for ( ;
	 notmuch_threads_valid (threads);
	 notmuch_threads_move_to_next (threads))
    {
	thread = notmuch_threads_get (threads);

	messages = notmuch_thread_get_toplevel_messages (thread);

	if (messages == NULL)
	    INTERNAL_ERROR ("Thread %s has no toplevel messages.\n",
			    notmuch_thread_get_thread_id (thread));

	status = show_messages (ctx, format, sp, messages, 0, params);
	if (status && !res)
	    res = status;

	notmuch_thread_destroy (thread);

    }

    sp->end (sp);

    return res != NOTMUCH_STATUS_SUCCESS;
}

enum {
    NOTMUCH_FORMAT_NOT_SPECIFIED,
    NOTMUCH_FORMAT_JSON,
    NOTMUCH_FORMAT_SEXP,
    NOTMUCH_FORMAT_TEXT,
    NOTMUCH_FORMAT_MBOX,
    NOTMUCH_FORMAT_RAW
};

static const notmuch_show_format_t format_json = {
    .new_sprinter = sprinter_json_create,
    .part = format_part_sprinter_entry,
};

static const notmuch_show_format_t format_sexp = {
    .new_sprinter = sprinter_sexp_create,
    .part = format_part_sprinter_entry,
};

static const notmuch_show_format_t format_text = {
    .new_sprinter = sprinter_text_create,
    .part = format_part_text,
};

static const notmuch_show_format_t format_mbox = {
    .new_sprinter = sprinter_text_create,
    .part = format_part_mbox,
};

static const notmuch_show_format_t format_raw = {
    .new_sprinter = sprinter_text_create,
    .part = format_part_raw,
};

static const notmuch_show_format_t *formatters[] = {
    [NOTMUCH_FORMAT_JSON] = &format_json,
    [NOTMUCH_FORMAT_SEXP] = &format_sexp,
    [NOTMUCH_FORMAT_TEXT] = &format_text,
    [NOTMUCH_FORMAT_MBOX] = &format_mbox,
    [NOTMUCH_FORMAT_RAW] = &format_raw,
};

int
notmuch_show_command (notmuch_config_t *config, int argc, char *argv[])
{
    notmuch_database_t *notmuch;
    notmuch_query_t *query;
    char *query_string;
    int opt_index, ret;
    const notmuch_show_format_t *formatter;
    sprinter_t *sprinter;
    notmuch_show_params_t params = {
	.part = -1,
	.omit_excluded = true,
	.output_body = true,
	.crypto = { .decrypt = NOTMUCH_DECRYPT_AUTO },
    };
    int format = NOTMUCH_FORMAT_NOT_SPECIFIED;
    bool exclude = true;
    bool entire_thread_set = false;
    bool single_message;

    notmuch_opt_desc_t options[] = {
	{ .opt_keyword = &format, .name = "format", .keywords =
	  (notmuch_keyword_t []){ { "json", NOTMUCH_FORMAT_JSON },
				  { "text", NOTMUCH_FORMAT_TEXT },
				  { "sexp", NOTMUCH_FORMAT_SEXP },
				  { "mbox", NOTMUCH_FORMAT_MBOX },
				  { "raw", NOTMUCH_FORMAT_RAW },
				  { 0, 0 } } },
	{ .opt_int = &notmuch_format_version, .name = "format-version" },
	{ .opt_bool = &exclude, .name = "exclude" },
	{ .opt_bool = &params.entire_thread, .name = "entire-thread",
	  .present = &entire_thread_set },
	{ .opt_int = &params.part, .name = "part" },
	{ .opt_keyword = (int*)(&params.crypto.decrypt), .name = "decrypt",
	  .keyword_no_arg_value = "true", .keywords =
	  (notmuch_keyword_t []){ { "false", NOTMUCH_DECRYPT_FALSE },
				  { "auto", NOTMUCH_DECRYPT_AUTO },
				  { "true", NOTMUCH_DECRYPT_NOSTASH },
				  { 0, 0 } } },
	{ .opt_bool = &params.crypto.verify, .name = "verify" },
	{ .opt_bool = &params.output_body, .name = "body" },
	{ .opt_bool = &params.include_html, .name = "include-html" },
	{ .opt_inherit = notmuch_shared_options },
	{ }
    };

    opt_index = parse_arguments (argc, argv, options, 1);
    if (opt_index < 0)
	return EXIT_FAILURE;

    notmuch_process_shared_options (argv[0]);

    /* explicit decryption implies verification */
    if (params.crypto.decrypt == NOTMUCH_DECRYPT_NOSTASH)
	params.crypto.verify = true;

    /* specifying a part implies single message display */
    single_message = params.part >= 0;

    if (format == NOTMUCH_FORMAT_NOT_SPECIFIED) {
	/* if part was requested and format was not specified, use format=raw */
	if (params.part >= 0)
	    format = NOTMUCH_FORMAT_RAW;
	else
	    format = NOTMUCH_FORMAT_TEXT;
    }

    if (format == NOTMUCH_FORMAT_MBOX) {
	if (params.part > 0) {
	    fprintf (stderr, "Error: specifying parts is incompatible with mbox output format.\n");
	    return EXIT_FAILURE;
	}
    } else if (format == NOTMUCH_FORMAT_RAW) {
	/* raw format only supports single message display */
	single_message = true;
    }

    notmuch_exit_if_unsupported_format ();

    /* Default is entire-thread = false except for format=json and
     * format=sexp. */
    if (! entire_thread_set &&
	(format == NOTMUCH_FORMAT_JSON || format == NOTMUCH_FORMAT_SEXP))
	params.entire_thread = true;

    if (!params.output_body) {
	if (params.part > 0) {
	    fprintf (stderr, "Warning: --body=false is incompatible with --part > 0. Disabling.\n");
	    params.output_body = true;
	} else {
	    if (format != NOTMUCH_FORMAT_JSON && format != NOTMUCH_FORMAT_SEXP)
		fprintf (stderr,
			 "Warning: --body=false only implemented for format=json and format=sexp\n");
	}
    }

    if (params.include_html &&
        (format != NOTMUCH_FORMAT_JSON && format != NOTMUCH_FORMAT_SEXP)) {
	fprintf (stderr, "Warning: --include-html only implemented for format=json and format=sexp\n");
    }

    query_string = query_string_from_args (config, argc-opt_index, argv+opt_index);
    if (query_string == NULL) {
	fprintf (stderr, "Out of memory\n");
	return EXIT_FAILURE;
    }

    if (*query_string == '\0') {
	fprintf (stderr, "Error: notmuch show requires at least one search term.\n");
	return EXIT_FAILURE;
    }

#if (GMIME_MAJOR_VERSION < 3)
    params.crypto.gpgpath = notmuch_config_get_crypto_gpg_path (config);
#endif

    if (notmuch_database_open (notmuch_config_get_database_path (config),
			       NOTMUCH_DATABASE_MODE_READ_ONLY, &notmuch))
	return EXIT_FAILURE;

    notmuch_exit_if_unmatched_db_uuid (notmuch);

    query = notmuch_query_create (notmuch, query_string);
    if (query == NULL) {
	fprintf (stderr, "Out of memory\n");
	return EXIT_FAILURE;
    }

    /* Create structure printer. */
    formatter = formatters[format];
    sprinter = formatter->new_sprinter(config, stdout);

    params.out_stream = g_mime_stream_stdout_new ();

    /* If a single message is requested we do not use search_excludes. */
    if (single_message) {
	ret = do_show_single (config, query, formatter, sprinter, &params);
    } else {
	/* We always apply set the exclude flag. The
	 * exclude=true|false option controls whether or not we return
	 * threads that only match in an excluded message */
	const char **search_exclude_tags;
	size_t search_exclude_tags_length;
	unsigned int i;
	notmuch_status_t status;

	search_exclude_tags = notmuch_config_get_search_exclude_tags
	    (config, &search_exclude_tags_length);

	for (i = 0; i < search_exclude_tags_length; i++) {
	    status = notmuch_query_add_tag_exclude (query, search_exclude_tags[i]);
	    if (status && status != NOTMUCH_STATUS_IGNORED) {
		print_status_query ("notmuch show", query, status);
		ret = -1;
		goto DONE;
	    }
	}

	if (exclude == false) {
	    notmuch_query_set_omit_excluded (query, false);
	    params.omit_excluded = false;
	}

	ret = do_show (config, query, formatter, sprinter, &params);
    }

 DONE:
    g_mime_stream_flush (params.out_stream);
    g_object_unref (params.out_stream);

    _notmuch_crypto_cleanup (&params.crypto);
    notmuch_query_destroy (query);
    notmuch_database_destroy (notmuch);

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
