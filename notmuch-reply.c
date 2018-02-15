/* notmuch - Not much of an email program, (just index and search)
 *
 * Copyright © 2009 Carl Worth
 * Copyright © 2009 Keith Packard
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
 * Authors: Carl Worth <cworth@cworth.org>
 *	    Keith Packard <keithp@keithp.com>
 */

#include "notmuch-client.h"
#include "string-util.h"
#include "sprinter.h"

static void
show_reply_headers (GMimeStream *stream, GMimeMessage *message)
{
    /* Output RFC 2822 formatted (and RFC 2047 encoded) headers. */
    if (g_mime_object_write_to_stream (GMIME_OBJECT(message), stream) < 0) {
	INTERNAL_ERROR("failed to write headers to stdout\n");
    }
}

static void
format_part_reply (GMimeStream *stream, mime_node_t *node)
{
    int i;

    if (node->envelope_file) {
	g_mime_stream_printf (stream, "On %s, %s wrote:\n",
			      notmuch_message_get_header (node->envelope_file, "date"),
			      notmuch_message_get_header (node->envelope_file, "from"));
    } else if (GMIME_IS_MESSAGE (node->part)) {
	GMimeMessage *message = GMIME_MESSAGE (node->part);
	char *recipients_string;

	g_mime_stream_printf (stream, "> From: %s\n", g_mime_message_get_from_string (message));
	recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_TO);
	if (recipients_string)
	    g_mime_stream_printf (stream, "> To: %s\n",
				  recipients_string);
	g_free (recipients_string);
	recipients_string = g_mime_message_get_address_string (message, GMIME_ADDRESS_TYPE_CC);
	if (recipients_string)
	    g_mime_stream_printf (stream, "> Cc: %s\n",
				  recipients_string);
	g_free (recipients_string);
	g_mime_stream_printf (stream, "> Subject: %s\n", g_mime_message_get_subject (message));
	g_mime_stream_printf (stream, "> Date: %s\n", g_mime_message_get_date_string (node, message));
	g_mime_stream_printf (stream, ">\n");
    } else if (GMIME_IS_PART (node->part)) {
	GMimeContentType *content_type = g_mime_object_get_content_type (node->part);
	GMimeContentDisposition *disposition = g_mime_object_get_content_disposition (node->part);

	if (g_mime_content_type_is_type (content_type, "application", "pgp-encrypted") ||
	    g_mime_content_type_is_type (content_type, "application", "pgp-signature")) {
	    /* Ignore PGP/MIME cruft parts */
	} else if (g_mime_content_type_is_type (content_type, "text", "*") &&
		   !g_mime_content_type_is_type (content_type, "text", "html")) {
	    show_text_part_content (node->part, stream, NOTMUCH_SHOW_TEXT_PART_REPLY);
	} else if (disposition &&
		   strcasecmp (g_mime_content_disposition_get_disposition (disposition),
			       GMIME_DISPOSITION_ATTACHMENT) == 0) {
	    const char *filename = g_mime_part_get_filename (GMIME_PART (node->part));
	    g_mime_stream_printf (stream, "Attachment: %s (%s)\n", filename,
				  g_mime_content_type_to_string (content_type));
	} else {
	    g_mime_stream_printf (stream, "Non-text part: %s\n",
				  g_mime_content_type_to_string (content_type));
	}
    }

    for (i = 0; i < node->nchildren; i++)
	format_part_reply (stream, mime_node_child (node, i));
}

typedef enum {
    USER_ADDRESS_IN_STRING,
    STRING_IN_USER_ADDRESS,
    STRING_IS_USER_ADDRESS,
} address_match_t;

/* Match given string against given address according to mode. */
static bool
match_address (const char *str, const char *address, address_match_t mode)
{
    switch (mode) {
    case USER_ADDRESS_IN_STRING:
	return strcasestr (str, address) != NULL;
    case STRING_IN_USER_ADDRESS:
	return strcasestr (address, str) != NULL;
    case STRING_IS_USER_ADDRESS:
	return strcasecmp (address, str) == 0;
    }

    return false;
}

/* Match given string against user's configured "primary" and "other"
 * addresses according to mode. */
static const char *
address_match (const char *str, notmuch_config_t *config, address_match_t mode)
{
    const char *primary;
    const char **other;
    size_t i, other_len;

    if (!str || *str == '\0')
	return NULL;

    primary = notmuch_config_get_user_primary_email (config);
    if (match_address (str, primary, mode))
	return primary;

    other = notmuch_config_get_user_other_email (config, &other_len);
    for (i = 0; i < other_len; i++) {
	if (match_address (str, other[i], mode))
	    return other[i];
    }

    return NULL;
}

/* Does the given string contain an address configured as one of the
 * user's "primary" or "other" addresses. If so, return the matching
 * address, NULL otherwise. */
static const char *
user_address_in_string (const char *str, notmuch_config_t *config)
{
    return address_match (str, config, USER_ADDRESS_IN_STRING);
}

/* Do any of the addresses configured as one of the user's "primary"
 * or "other" addresses contain the given string. If so, return the
 * matching address, NULL otherwise. */
static const char *
string_in_user_address (const char *str, notmuch_config_t *config)
{
    return address_match (str, config, STRING_IN_USER_ADDRESS);
}

/* Is the given address configured as one of the user's "primary" or
 * "other" addresses. */
static bool
address_is_users (const char *address, notmuch_config_t *config)
{
    return address_match (address, config, STRING_IS_USER_ADDRESS) != NULL;
}

/* Scan addresses in 'list'.
 *
 * If 'message' is non-NULL, then for each address in 'list' that is
 * not configured as one of the user's addresses in 'config', add that
 * address to 'message' as an address of 'type'.
 *
 * If 'user_from' is non-NULL and *user_from is NULL, *user_from will
 * be set to the first address encountered in 'list' that is the
 * user's address.
 *
 * Return the number of addresses added to 'message'. (If 'message' is
 * NULL, the function returns 0 by definition.)
 */
static unsigned int
scan_address_list (InternetAddressList *list,
		   notmuch_config_t *config,
		   GMimeMessage *message,
		   GMimeRecipientType type,
		   const char **user_from)
{
    InternetAddress *address;
    int i;
    unsigned int n = 0;

    if (list == NULL)
	return 0;

    for (i = 0; i < internet_address_list_length (list); i++) {
	address = internet_address_list_get_address (list, i);
	if (INTERNET_ADDRESS_IS_GROUP (address)) {
	    InternetAddressGroup *group;
	    InternetAddressList *group_list;

	    group = INTERNET_ADDRESS_GROUP (address);
	    group_list = internet_address_group_get_members (group);
	    n += scan_address_list (group_list, config, message, type, user_from);
	} else {
	    InternetAddressMailbox *mailbox;
	    const char *name;
	    const char *addr;

	    mailbox = INTERNET_ADDRESS_MAILBOX (address);

	    name = internet_address_get_name (address);
	    addr = internet_address_mailbox_get_addr (mailbox);

	    if (address_is_users (addr, config)) {
		if (user_from && *user_from == NULL)
		    *user_from = addr;
	    } else if (message) {
		g_mime_message_add_recipient (message, type, name, addr);
		n++;
	    }
	}
    }

    return n;
}

/* Does the address in the Reply-To header of 'message' already appear
 * in either the 'To' or 'Cc' header of the message?
 */
static bool
reply_to_header_is_redundant (GMimeMessage *message,
			      InternetAddressList *reply_to_list)
{
    const char *addr, *reply_to;
    InternetAddress *address;
    InternetAddressMailbox *mailbox;
    InternetAddressList *recipients;
    bool ret = false;
    int i;

    if (reply_to_list == NULL ||
	internet_address_list_length (reply_to_list) != 1)
	return 0;

    address = internet_address_list_get_address (reply_to_list, 0);
    if (INTERNET_ADDRESS_IS_GROUP (address))
	return 0;

    mailbox = INTERNET_ADDRESS_MAILBOX (address);
    reply_to = internet_address_mailbox_get_addr (mailbox);

    recipients = g_mime_message_get_all_recipients (message);

    for (i = 0; i < internet_address_list_length (recipients); i++) {
	address = internet_address_list_get_address (recipients, i);
	if (INTERNET_ADDRESS_IS_GROUP (address))
	    continue;

	mailbox = INTERNET_ADDRESS_MAILBOX (address);
	addr = internet_address_mailbox_get_addr (mailbox);
	if (strcmp (addr, reply_to) == 0) {
	    ret = true;
	    break;
	}
    }

    g_object_unref (G_OBJECT (recipients));

    return ret;
}

static InternetAddressList *get_sender(GMimeMessage *message)
{
    InternetAddressList *reply_to_list;

    reply_to_list = g_mime_message_get_reply_to_list (message);
    if (reply_to_list &&
	internet_address_list_length (reply_to_list) > 0) {
        /*
	 * Some mailing lists munge the Reply-To header despite it
	 * being A Bad Thing, see
	 * http://marc.merlins.org/netrants/reply-to-harmful.html
	 *
	 * The munging is easy to detect, because it results in a
	 * redundant reply-to header, (with an address that already
	 * exists in either To or Cc). So in this case, we ignore the
	 * Reply-To field and use the From header. This ensures the
	 * original sender will get the reply even if not subscribed
	 * to the list. Note that the address in the Reply-To header
	 * will always appear in the reply if reply_all is true.
	 */
	if (! reply_to_header_is_redundant (message, reply_to_list))
	    return reply_to_list;

	g_mime_2_6_unref (G_OBJECT (reply_to_list));
    }

    return g_mime_message_get_from (message);
}

static InternetAddressList *get_to(GMimeMessage *message)
{
    return g_mime_message_get_addresses (message, GMIME_ADDRESS_TYPE_TO);
}

static InternetAddressList *get_cc(GMimeMessage *message)
{
    return g_mime_message_get_addresses (message, GMIME_ADDRESS_TYPE_CC);
}

static InternetAddressList *get_bcc(GMimeMessage *message)
{
    return g_mime_message_get_addresses (message, GMIME_ADDRESS_TYPE_BCC);
}

/* Augment the recipients of 'reply' from the "Reply-to:", "From:",
 * "To:", "Cc:", and "Bcc:" headers of 'message'.
 *
 * If 'reply_all' is true, use sender and all recipients, otherwise
 * scan the headers for the first that contains something other than
 * the user's addresses and add the recipients from this header
 * (typically this would be reply-to-sender, but also handles reply to
 * user's own message in a sensible way).
 *
 * If any of the user's addresses were found in these headers, the
 * first of these returned, otherwise NULL is returned.
 */
static const char *
add_recipients_from_message (GMimeMessage *reply,
			     notmuch_config_t *config,
			     GMimeMessage *message,
			     bool reply_all)
{

    /* There is a memory leak here with gmime-2.6 because get_sender
     * returns a newly allocated list, while the others return
     * references to libgmime owned data. This leak will be fixed with
     * the transition to gmime-3.0.
     */
    struct {
	InternetAddressList * (*get_header)(GMimeMessage *message);
	GMimeRecipientType recipient_type;
    } reply_to_map[] = {
	{ get_sender,	GMIME_ADDRESS_TYPE_TO },
	{ get_to,	GMIME_ADDRESS_TYPE_TO },
	{ get_cc,	GMIME_ADDRESS_TYPE_CC },
	{ get_bcc,	GMIME_ADDRESS_TYPE_BCC },
    };
    const char *from_addr = NULL;
    unsigned int i;
    unsigned int n = 0;

    for (i = 0; i < ARRAY_SIZE (reply_to_map); i++) {
	InternetAddressList *recipients;

	recipients = reply_to_map[i].get_header (message);

	n += scan_address_list (recipients, config, reply,
				reply_to_map[i].recipient_type, &from_addr);

	if (!reply_all && n) {
	    /* Stop adding new recipients in reply-to-sender mode if
	     * we have added some recipient(s) above.
	     *
	     * This also handles the case of user replying to his own
	     * message, where reply-to/from is not a recipient. In
	     * this case there may be more than one recipient even if
	     * not replying to all.
	     */
	    reply = NULL;

	    /* From address and some recipients are enough, bail out. */
	    if (from_addr)
		break;
	}
    }

    return from_addr;
}

/*
 * Look for the user's address in " for <email@add.res>" in the
 * received headers.
 *
 * Return the address that was found, if any, and NULL otherwise.
 */
static const char *
guess_from_in_received_for (notmuch_config_t *config, const char *received)
{
    const char *ptr;

    ptr = strstr (received, " for ");
    if (! ptr)
	return NULL;

    return user_address_in_string (ptr, config);
}

/*
 * Parse all the " by MTA ..." parts in received headers to guess the
 * email address that this was originally delivered to.
 *
 * Extract just the MTA here by removing leading whitespace and
 * assuming that the MTA name ends at the next whitespace. Test for
 * *(by+4) to be non-'\0' to make sure there's something there at all
 * - and then assume that the first whitespace delimited token that
 * follows is the receiving system in this step of the receive chain.
 *
 * Return the address that was found, if any, and NULL otherwise.
 */
static const char *
guess_from_in_received_by (notmuch_config_t *config, const char *received)
{
    const char *addr;
    const char *by = received;
    char *domain, *tld, *mta, *ptr, *token;

    while ((by = strstr (by, " by ")) != NULL) {
	by += 4;
	if (*by == '\0')
	    break;
	mta = xstrdup (by);
	token = strtok(mta," \t");
	if (token == NULL) {
	    free (mta);
	    break;
	}
	/*
	 * Now extract the last two components of the MTA host name as
	 * domain and tld.
	 */
	domain = tld = NULL;
	while ((ptr = strsep (&token, ". \t")) != NULL) {
	    if (*ptr == '\0')
		continue;
	    domain = tld;
	    tld = ptr;
	}

	if (domain) {
	    /*
	     * Recombine domain and tld and look for it among the
	     * configured email addresses. This time we have a known
	     * domain name and nothing else - so the test is the other
	     * way around: we check if this is a substring of one of
	     * the email addresses.
	     */
	    *(tld - 1) = '.';

	    addr = string_in_user_address (domain, config);
	    if (addr) {
		free (mta);
		return addr;
	    }
	}
	free (mta);
    }

    return NULL;
}

/*
 * Get the concatenated Received: headers and search from the front
 * (last Received: header added) and try to extract from them
 * indications to which email address this message was delivered.
 *
 * The Received: header is special in our get_header function and is
 * always concatenated.
 *
 * Return the address that was found, if any, and NULL otherwise.
 */
static const char *
guess_from_in_received_headers (notmuch_config_t *config,
				notmuch_message_t *message)
{
    const char *received, *addr;
    char *sanitized;

    received = notmuch_message_get_header (message, "received");
    if (! received)
	return NULL;

    sanitized = sanitize_string (NULL, received);
    if (! sanitized)
	return NULL;

    addr = guess_from_in_received_for (config, sanitized);
    if (! addr)
	addr = guess_from_in_received_by (config, sanitized);

    talloc_free (sanitized);

    return addr;
}

/*
 * Try to find user's email address in one of the extra To-like
 * headers: Envelope-To, X-Original-To, and Delivered-To (searched in
 * that order).
 *
 * Return the address that was found, if any, and NULL otherwise.
 */
static const char *
get_from_in_to_headers (notmuch_config_t *config, notmuch_message_t *message)
{
    size_t i;
    const char *tohdr, *addr;
    const char *to_headers[] = {
	"Envelope-to",
	"X-Original-To",
	"Delivered-To",
    };

    for (i = 0; i < ARRAY_SIZE (to_headers); i++) {
	tohdr = notmuch_message_get_header (message, to_headers[i]);

	/* Note: tohdr potentially contains a list of email addresses. */
	addr = user_address_in_string (tohdr, config);
	if (addr)
	    return addr;
    }

    return NULL;
}

static GMimeMessage *
create_reply_message(void *ctx,
		     notmuch_config_t *config,
		     notmuch_message_t *message,
		     GMimeMessage *mime_message,
		     bool reply_all,
		     bool limited)
{
    const char *subject, *from_addr = NULL;
    const char *in_reply_to, *orig_references, *references;

    /*
     * Use the below header order for limited headers, "pretty" order
     * otherwise.
     */
    GMimeMessage *reply = g_mime_message_new (limited ? 0 : 1);
    if (reply == NULL) {
	fprintf (stderr, "Out of memory\n");
	return NULL;
    }

    in_reply_to = talloc_asprintf (ctx, "<%s>",
				   notmuch_message_get_message_id (message));

    g_mime_object_set_header (GMIME_OBJECT (reply), "In-Reply-To", in_reply_to);

    orig_references = notmuch_message_get_header (message, "references");
    if (orig_references && *orig_references)
	references = talloc_asprintf (ctx, "%s %s", orig_references,
				      in_reply_to);
    else
	references = talloc_strdup (ctx, in_reply_to);

    g_mime_object_set_header (GMIME_OBJECT (reply), "References", references);

    from_addr = add_recipients_from_message (reply, config,
					     mime_message, reply_all);

    /* The above is all that is needed for limited headers. */
    if (limited)
	return reply;

    /*
     * Sadly, there is no standard way to find out to which email
     * address a mail was delivered - what is in the headers depends
     * on the MTAs used along the way.
     *
     * If none of the user's email addresses are in the To: or Cc:
     * headers, we try a number of heuristics which hopefully will
     * answer this question.
     *
     * First, check for Envelope-To:, X-Original-To:, and
     * Delivered-To: headers.
     */
    if (from_addr == NULL)
	from_addr = get_from_in_to_headers (config, message);

    /*
     * Check for a (for <email@add.res>) clause in Received: headers,
     * and the domain part of known email addresses in the 'by' part
     * of Received: headers
     */
    if (from_addr == NULL)
	from_addr = guess_from_in_received_headers (config, message);

    /* Default to user's primary address. */
    if (from_addr == NULL)
	from_addr = notmuch_config_get_user_primary_email (config);

    from_addr = talloc_asprintf (ctx, "%s <%s>",
				 notmuch_config_get_user_name (config),
				 from_addr);
    g_mime_object_set_header (GMIME_OBJECT (reply), "From", from_addr);

    subject = notmuch_message_get_header (message, "subject");
    if (subject) {
	if (strncasecmp (subject, "Re:", 3))
	    subject = talloc_asprintf (ctx, "Re: %s", subject);
	g_mime_message_set_subject (reply, subject);
    }

    return reply;
}

enum {
    FORMAT_DEFAULT,
    FORMAT_JSON,
    FORMAT_SEXP,
    FORMAT_HEADERS_ONLY,
};

static int do_reply(notmuch_config_t *config,
		    notmuch_query_t *query,
		    notmuch_show_params_t *params,
		    int format,
		    bool reply_all)
{
    GMimeMessage *reply;
    mime_node_t *node;
    notmuch_messages_t *messages;
    notmuch_message_t *message;
    notmuch_status_t status;
    struct sprinter *sp = NULL;

    if (format == FORMAT_JSON || format == FORMAT_SEXP) {
	unsigned count;

	status = notmuch_query_count_messages (query, &count);
	if (print_status_query ("notmuch reply", query, status))
	    return 1;

	if (count != 1) {
	    fprintf (stderr, "Error: search term did not match precisely one message (matched %u messages).\n", count);
	    return 1;
	}

	if (format == FORMAT_JSON)
	    sp = sprinter_json_create (config, stdout);
	else
	    sp = sprinter_sexp_create (config, stdout);
    }

    status = notmuch_query_search_messages (query, &messages);
    if (print_status_query ("notmuch reply", query, status))
	return 1;

    for (;
	 notmuch_messages_valid (messages);
	 notmuch_messages_move_to_next (messages))
    {
	message = notmuch_messages_get (messages);

	if (mime_node_open (config, message, &params->crypto, &node))
	    return 1;

	reply = create_reply_message (config, config, message,
				      GMIME_MESSAGE (node->part), reply_all,
				      format == FORMAT_HEADERS_ONLY);
	if (!reply)
	    return 1;

	if (format == FORMAT_JSON || format == FORMAT_SEXP) {
	    sp->begin_map (sp);

	    /* The headers of the reply message we've created */
	    sp->map_key (sp, "reply-headers");
	    format_headers_sprinter (sp, reply, true);

	    /* Start the original */
	    sp->map_key (sp, "original");
	    format_part_sprinter (config, sp, node, true, false);

	    /* End */
	    sp->end (sp);
	} else {
	    GMimeStream *stream_stdout = stream_stdout = g_mime_stream_stdout_new ();
	    if (stream_stdout) {
		show_reply_headers (stream_stdout, reply);
		if (format == FORMAT_DEFAULT)
		    format_part_reply (stream_stdout, node);
	    }
	    g_mime_stream_flush (stream_stdout);
	    g_object_unref(stream_stdout);
	}

	g_object_unref (G_OBJECT (reply));
	talloc_free (node);

	notmuch_message_destroy (message);
    }

    return 0;
}

int
notmuch_reply_command (notmuch_config_t *config, int argc, char *argv[])
{
    notmuch_database_t *notmuch;
    notmuch_query_t *query;
    char *query_string;
    int opt_index;
    notmuch_show_params_t params = {
	.part = -1,
	.crypto = { .decrypt = NOTMUCH_DECRYPT_AUTO },
    };
    int format = FORMAT_DEFAULT;
    int reply_all = true;

    notmuch_opt_desc_t options[] = {
	{ .opt_keyword = &format, .name = "format", .keywords =
	  (notmuch_keyword_t []){ { "default", FORMAT_DEFAULT },
				  { "json", FORMAT_JSON },
				  { "sexp", FORMAT_SEXP },
				  { "headers-only", FORMAT_HEADERS_ONLY },
				  { 0, 0 } } },
	{ .opt_int = &notmuch_format_version, .name = "format-version" },
	{ .opt_keyword = &reply_all, .name = "reply-to", .keywords =
	  (notmuch_keyword_t []){ { "all", true },
				  { "sender", false },
				  { 0, 0 } } },
	{ .opt_keyword = (int*)(&params.crypto.decrypt), .name = "decrypt",
	  .keyword_no_arg_value = "true", .keywords =
	  (notmuch_keyword_t []){ { "false", NOTMUCH_DECRYPT_FALSE },
				  { "auto", NOTMUCH_DECRYPT_AUTO },
				  { "true", NOTMUCH_DECRYPT_NOSTASH },
				  { 0, 0 } } },
	{ .opt_inherit = notmuch_shared_options },
	{ }
    };

    opt_index = parse_arguments (argc, argv, options, 1);
    if (opt_index < 0)
	return EXIT_FAILURE;

    notmuch_process_shared_options (argv[0]);

    notmuch_exit_if_unsupported_format ();

    query_string = query_string_from_args (config, argc-opt_index, argv+opt_index);
    if (query_string == NULL) {
	fprintf (stderr, "Out of memory\n");
	return EXIT_FAILURE;
    }

    if (*query_string == '\0') {
	fprintf (stderr, "Error: notmuch reply requires at least one search term.\n");
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

    if (do_reply (config, query, &params, format, reply_all) != 0)
	return EXIT_FAILURE;

    _notmuch_crypto_cleanup (&params.crypto);
    notmuch_query_destroy (query);
    notmuch_database_destroy (notmuch);

    return EXIT_SUCCESS;
}
