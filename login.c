/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <debug.h>
#include <glib/gi18n.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "login-private.h"
#include "chime-connection-private.h"

#define SEARCH_FORM  "//form[@id='picker_email']"
#define TOKEN_REGEX  "['\"]chime://sso_sessions\\?Token=([^'\"]+)['\"]"

gpointer chime_login_extend_state(gpointer state, gsize size, GDestroyNotify destroy)
{
	gpointer new;

	new = g_realloc(state, size);
	memset((ChimeLogin *) new + 1, 0, size - sizeof(ChimeLogin));
	((ChimeLogin *) new)->release_sub = destroy;
	return new;
}

void chime_login_free_state(ChimeLogin *state)
{
	g_return_if_fail(state != NULL);

	if (state->release_sub != NULL)
		state->release_sub(state);
	
	soup_session_abort(state->session);
	g_object_unref(state->session);
	g_object_unref(state->connection);
	g_free(state);
}

static void fail(ChimeLogin *state, GError *error)
{
	g_assert(error != NULL);
	purple_debug_error("chime", "Login failure: %s", error->message);
	chime_connection_fail_error(state->connection, error);
	g_error_free(error);
	chime_login_free_state(state);
}

void chime_login_cancel_ui(ChimeLogin *state, gpointer foo)
{
	fail(state, g_error_new(CHIME_ERROR, CHIME_ERROR_AUTH_FAILED,
				_("Authentication canceled by the user")));
}

void chime_login_cancel_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	chime_login_cancel_ui(data, NULL);
}

void chime_login_request_failed(gpointer state, const gchar *location, SoupMessage *msg)
{
	purple_debug_error("chime", "%s: Server returned error %d %s", location,
			   msg->status_code, msg->reason_phrase);
	fail(state, g_error_new(CHIME_ERROR, CHIME_ERROR_REQUEST_FAILED,
				_("A request failed during authentication")));
}

void chime_login_bad_response(gpointer state, const gchar *fmt, ...)
{
	va_list args;
	
	va_start(args, fmt);
	fail(state, g_error_new_valist(CHIME_ERROR, CHIME_ERROR_BAD_RESPONSE, fmt, args));
	va_end(args);
}

static gboolean xpath_exists(xmlXPathContext *ctx, const gchar *fmt, ...)
{
	gboolean found;
	gchar *expression;
	va_list args;
	xmlXPathObject *results;
	
	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	results = xmlXPathEval(BAD_CAST expression, ctx);
	found = results != NULL && results->type == XPATH_NODESET &&
		results->nodesetval != NULL && results->nodesetval->nodeNr > 0;
	xmlXPathFreeObject(results);
	g_free(expression);
	return found;
}

static xmlNode **xpath_nodes(xmlXPathContext *ctx, guint *count, const gchar *fmt, ...)
{
	gchar *expression;
	va_list args;
	xmlNode **nodes;
	xmlXPathObject *results;

	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	results = xmlXPathEval(BAD_CAST expression, ctx);
	if (results != NULL && results->type == XPATH_NODESET &&
	    results->nodesetval != NULL) {
		*count = (guint) results->nodesetval->nodeNr;
		nodes = g_memdup(results->nodesetval->nodeTab,
				 results->nodesetval->nodeNr * sizeof(xmlNode *));
	} else {
		*count = 0;
		nodes = NULL;
	}
	xmlXPathFreeObject(results);
	g_free(expression);
	return nodes;
}

static gchar *xpath_string(xmlXPathContext *ctx, const gchar *fmt, ...)
{
	gchar *expression, *wrapped, *value = NULL;
	va_list args;
	xmlXPathObject *results;

	va_start(args, fmt);
	expression = g_strdup_vprintf(fmt, args);
	va_end(args);
	wrapped = g_strdup_printf("string(%s)", expression);
	results = xmlXPathEval(BAD_CAST wrapped, ctx);
	if (results != NULL && results->type == XPATH_STRING)
		value = g_strdup((gchar *) results->stringval);
	xmlXPathFreeObject(results);
	g_free(wrapped);
	g_free(expression);
	return value;

}

static xmlDoc *parse_html(SoupMessage *msg)
{
	GHashTable *params;
	const gchar *ctype;
	gchar *url;
	xmlDoc *document = NULL;

	ctype = soup_message_headers_get_content_type(msg->response_headers, &params);
	if (g_strcmp0(ctype, "text/html") != 0 || !msg->response_body ||
	    msg->response_body->length <= 0) {
		purple_debug_error("chime", "Empty HTML response or unexpected content %s", ctype);
		goto out;
	}
	
	url = soup_uri_to_string(soup_message_get_uri(msg), FALSE);
	document = htmlReadMemory(msg->response_body->data,
				  msg->response_body->length,
				  url, g_hash_table_lookup(params, "charset"),
				  HTML_PARSE_NODEFDTD | HTML_PARSE_NOERROR |
				  HTML_PARSE_NOWARNING | HTML_PARSE_NONET |
				  HTML_PARSE_RECOVER);
	g_free(url);
 out:
	g_hash_table_destroy(params);
	return document;
}

gchar *chime_login_parse_regex(SoupMessage *msg, const gchar *regex, guint group)
{
	GMatchInfo *match;
	GRegex *matcher;
	gchar *text = NULL;

	if (!msg->response_body || msg->response_body->length <= 0) {
		purple_debug_error("chime", "Empty text response");
		return text;
	}
	
	matcher = g_regex_new(regex, 0, 0, NULL);
	if (g_regex_match_full(matcher, msg->response_body->data,
			       msg->response_body->length, 0, 0, &match, NULL))
		text = g_match_info_fetch(match, group);
	
	g_match_info_free(match);
	g_regex_unref(matcher);
	return text;
}

gchar **chime_login_parse_xpaths(SoupMessage *msg, guint count, ...)
{
	gchar **values = NULL;
	guint i;
	va_list args;
	xmlDoc *html;
	xmlXPathContext *ctx;

	html = parse_html(msg);
	if (html == NULL)
		return values;

	ctx = xmlXPathNewContext(html);
	if (ctx == NULL) {
		purple_debug_error("chime", "Failed to create XPath context to parse form");
		goto out;
	}

	values = g_new0(gchar *, count + 1);
	
	va_start(args, count);
	for (i = 0;  i < count;  i++)
		values[i] = xpath_string(ctx, va_arg(args, const gchar *));
	va_end(args);
 out:
	xmlXPathFreeContext(ctx);
	xmlFreeDoc(html);
	return values;
}

GHashTable *chime_login_parse_json_object(SoupMessage *msg)
{
	const gchar *ctype;
	GError *error = NULL;
	GHashTable *result = NULL;
	GList *members, *member;
	JsonNode *node;
	JsonObject *object;
	JsonParser *parser;

	ctype = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (g_strcmp0(ctype, "application/json") != 0 || !msg->response_body ||
	    msg->response_body->length <= 0) {
		purple_debug_error("chime", "Empty JSON response or unexpected content %s", ctype);
		return result;
	}
	
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, msg->response_body->data,
					msg->response_body->length, &error)) {
		purple_debug_error("chime", "JSON parsing error: %s", error->message);
		goto out;
	}
	
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(node)) {
		purple_debug_error("chime", "Unexpected JSON type %d", JSON_NODE_TYPE(node));
		goto out;
	}
	
	object = json_node_get_object(node);
	result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	
	members = json_object_get_members(object);
	for (member = g_list_first(members);  member != NULL;  member = member->next) {
		node = json_object_get_member(object, member->data);
		if (JSON_NODE_HOLDS_VALUE(node))
			g_hash_table_insert(result, g_strdup(member->data),
					    g_strdup(json_node_get_string(node)));
	}
	g_list_free(members);
 out:
	g_error_free(error);
	g_object_unref(parser);
	return result;
}

GHashTable *chime_login_parse_form(SoupMessage *msg, const gchar *form_xpath,
				   gchar **method, gchar **action,
				   gchar **email_name, gchar **password_name)
{
	GHashTable *params = NULL;
	gchar *form_action;
	guint i, n;
	xmlDoc *html;
	xmlNode **inputs;
	xmlXPathContext *ctx;

	g_return_val_if_fail(method != NULL && action != NULL, NULL);
	*method = *action = NULL;
	if (email_name != NULL)
		*email_name = NULL;
	if (password_name != NULL)
		*password_name = NULL;
	
	html = parse_html(msg);
	if (html == NULL)
		return params;

	ctx = xmlXPathNewContext(html);
	if (ctx == NULL) {
		purple_debug_error("chime", "Failed to create XPath context to parse form");
		goto out;
	}

	if (!xpath_exists(ctx, form_xpath)) {
		purple_debug_error("chime", "XPath query returned no results: %s", form_xpath);
		goto out;
	}
	
	*method = xpath_string(ctx, "%s/@method", form_xpath);
	if (*method == NULL) {
		*method = g_strdup(SOUP_METHOD_GET);
	} else {
		for (i = 0;  (*method)[i] != '\0';  i++)
			(*method)[i] = g_ascii_toupper((*method)[i]);
	}

	form_action = xpath_string(ctx, "%s/@action", form_xpath);
	if (form_action == NULL) {
		*action = soup_uri_to_string(soup_message_get_uri(msg), FALSE);
	} else {
		SoupURI *dst = soup_uri_new_with_base(soup_message_get_uri(msg), form_action);
		*action = soup_uri_to_string(dst, FALSE);
		soup_uri_free(dst);
	}

	if (email_name != NULL)
		*email_name = xpath_string(ctx, "%s//input[@type='email'][1]/@name",
					   form_xpath);
	if (password_name != NULL)
		*password_name = xpath_string(ctx, "%s//input[@type='password'][1]/@name",
					      form_xpath);

	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	inputs = xpath_nodes(ctx, &n, "%s//input[@type='hidden']", form_xpath);
	for (i = 0;  i < n;  i++) {
		gchar *name, *value;
		xmlChar *text;
		xmlAttr *attribute = xmlHasProp(inputs[i], BAD_CAST "name");
		if (attribute == NULL)
			continue;
		text = xmlNodeGetContent((xmlNode *) attribute);
		name = g_strdup((gchar *) text);  /* Avoid mixing allocators */
		xmlFree(text);
		attribute = xmlHasProp(inputs[i], BAD_CAST "value");
		if (attribute != NULL) {
			text = xmlNodeGetContent((xmlNode *) attribute);
			value = g_strdup((gchar *) text);
			xmlFree(text);
		} else {
			value = g_strdup("");
		}
		g_hash_table_insert(params, name, value);
	}
	
	g_free(inputs);
	g_free(form_action);
 out:
	xmlXPathFreeContext(ctx);
	xmlFreeDoc(html);
	return params;
}

void chime_login_token_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	gchar *token;

	chime_login_fail_on_error(msg, state);

	token = chime_login_parse_regex(msg, TOKEN_REGEX, 1);
	if (token == NULL) {
		purple_debug_error("chime", "Could not find session token in final login response");
		chime_login_bad_response(state, _("Unable to retrieve session token"));
		return;
	}
	
	chime_connection_set_session_token(state->connection, token);
	chime_connection_connect(state->connection);
	chime_login_free_state(state);
	g_free(token);
}

static void signin_search_result_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	GHashTable *provider_info;
	SoupMessage *next;
	SoupSessionCallback handler;
	SoupURI *destination;
	gchar *type, *path;

	if (msg->status_code == 400) {
		chime_login_bad_response(state, _("Invalid e-mail address <%s>"),
					 chime_login_account_email(state));
		return;
	}

	chime_login_fail_on_error(msg, state);
		
	provider_info = chime_login_parse_json_object(msg);
	if (provider_info == NULL) {
		chime_login_bad_response(state, _("Error parsing provider JSON"));
		return;
	}

	type = g_hash_table_lookup(provider_info, "provider");
	if (!g_strcmp0(type, "amazon")) {
		handler = chime_login_amazon;
	} else if (!g_strcmp0(type, "wd")) {
		handler = chime_login_warpdrive;
	} else {
		purple_debug_error("chime", "Unrecognized provider %s", type);
		chime_login_bad_response(state, _("Unknown login provider"));
		goto out;
	}

	path = g_hash_table_lookup(provider_info, "path");
	if (path == NULL) {
		purple_debug_error("chime", "Server did not provide a path");
		chime_login_bad_response(state, _("Incomplete provider response"));
		goto out;
	}

	destination = soup_uri_new_with_base(soup_message_get_uri(msg), path);
	next = soup_message_new_from_uri(SOUP_METHOD_GET, destination);
	soup_message_set_first_party(next, destination);
	soup_session_queue_message(session, next, handler, state);
	soup_uri_free(destination);
 out:
	g_hash_table_destroy(provider_info);
}

static void signin_page_cb(SoupSession *session, SoupMessage *msg, gpointer data)
{
	ChimeLogin *state = data;
	GHashTable *params;
	SoupMessage *next;
	gchar *method, *action, *email_name;

	chime_login_fail_on_error(msg, state);

	params = chime_login_parse_form(msg, SEARCH_FORM, &method, &action, &email_name, NULL);
	if (params == NULL || email_name == NULL) {
		chime_login_bad_response(state, _("Could not find provider search form"));
		goto out;
	}

	g_hash_table_insert(params, g_strdup(email_name), g_strdup(chime_login_account_email(state)));
	next = soup_form_request_new_from_hash(method, action, params);
	soup_session_queue_message(session, next, signin_search_result_cb, state);
 out:
	g_free(email_name);
	g_free(action);
	g_free(method);
	g_hash_table_destroy(params);
}

/*
 * Login process entry point.
 *
 * This is where the plugin initiates the authentication process.  Control is
 * transferred to this module until a connection is canceled or restarted once
 * we have the session token.
 */
void chime_initial_login(ChimeConnection *cxn)
{
	ChimeConnectionPrivate *priv;
	ChimeLogin *state;
	SoupMessage *msg;

	g_return_if_fail(CHIME_IS_CONNECTION(cxn));

	state = g_new0(ChimeLogin, 1);
	state->connection = g_object_ref(cxn);
	state->session = soup_session_new_with_options(SOUP_SESSION_ADD_FEATURE_BY_TYPE,
						       SOUP_TYPE_COOKIE_JAR, NULL);
	/* TODO: This needs to go somewhere else */
	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(state->session, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
	}
	priv = CHIME_CONNECTION_GET_PRIVATE(cxn);
	msg = soup_message_new(SOUP_METHOD_GET, priv->server);
	soup_session_queue_message(state->session, msg, signin_page_cb, state);
}
