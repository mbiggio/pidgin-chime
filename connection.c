/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *          Ignacio Casal Quinteiro <qignacio@amazon.com>
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

#include "connection.h"
#include "chime.h"

#include <glib/gi18n.h>

enum
{
    PROP_0,
    PROP_PURPLE_CONNECTION,
    PROP_SESSION_TOKEN,
    LAST_PROP
};

static GParamSpec *props[LAST_PROP];

G_DEFINE_QUARK(chime-connection-error-quark, chime_connection_error)
G_DEFINE_TYPE(ChimeConnection, chime_connection, G_TYPE_OBJECT)

static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg);

static void
chime_connection_finalize(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	g_free(self->session_token);

	G_OBJECT_CLASS(chime_connection_parent_class)->finalize(object);
}

static void
chime_connection_dispose(GObject *object)
{
	ChimeConnection *self = CHIME_CONNECTION(object);
	GList *l;

	if (self->soup_sess) {
		soup_session_abort(self->soup_sess);
	}
	g_clear_object(&self->soup_sess);

	chime_destroy_juggernaut(self);
	chime_destroy_buddies(self);
	chime_destroy_rooms(self);
	chime_destroy_conversations(self);
	chime_destroy_chats(self);

	g_clear_pointer(&self->reg_node, json_node_unref);

	// FIXME: change to use g_list_free_full
	while ( (l = g_list_first(self->msg_queue)) ) {
		struct chime_msg *cmsg = l->data;

		g_object_unref(cmsg->msg);
		g_free(cmsg);
		self->msg_queue = g_list_remove(self->msg_queue, cmsg);
	}
	self->msg_queue = NULL;
	
	purple_connection_set_protocol_data(self->prpl_conn, NULL);

	G_OBJECT_CLASS(chime_connection_parent_class)->dispose(object);
}

static void
chime_connection_get_property(GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	switch (prop_id) {
	case PROP_PURPLE_CONNECTION:
		g_value_set_pointer(value, self->prpl_conn);
		break;
	case PROP_SESSION_TOKEN:
		g_value_set_string(value, self->session_token);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_set_property(GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	ChimeConnection *self = CHIME_CONNECTION(object);

	switch (prop_id) {
	case PROP_PURPLE_CONNECTION:
		self->prpl_conn = g_value_get_pointer(value);
		break;
	case PROP_SESSION_TOKEN:
		self->session_token = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
chime_connection_class_init(ChimeConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = chime_connection_finalize;
	object_class->dispose = chime_connection_dispose;
	object_class->get_property = chime_connection_get_property;
	object_class->set_property = chime_connection_set_property;

	props[PROP_PURPLE_CONNECTION] =
		g_param_spec_pointer("purple-connection",
				     "purple connection",
				     "purple connection",
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_STRINGS);

	props[PROP_SESSION_TOKEN] =
		g_param_spec_string("session-token",
				    "session token",
				    "session token",
				    NULL,
				    G_PARAM_READWRITE |
				    G_PARAM_CONSTRUCT |
				    G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, LAST_PROP, props);
}

static void
chime_connection_init(ChimeConnection *self)
{
	self->soup_sess = soup_session_new();

	if (getenv("CHIME_DEBUG") && atoi(getenv("CHIME_DEBUG")) > 0) {
		SoupLogger *l = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
		soup_session_add_feature(self->soup_sess, SOUP_SESSION_FEATURE(l));
		g_object_unref(l);
		g_object_set(self->soup_sess, "ssl-strict", FALSE, NULL);
	}
}

ChimeConnection *
chime_connection_new(PurpleConnection *connection)
{
	return g_object_new (CHIME_TYPE_CONNECTION,
	                     "purple-connection", connection,
	                     NULL);
}

static gboolean parse_regnode(ChimeConnection *self, JsonNode *regnode)
{
	JsonObject *obj = json_node_get_object(self->reg_node);
	JsonNode *node, *sess_node = json_object_get_member(obj, "Session");
	const gchar *sess_tok;
	const gchar *display_name;

	if (!sess_node)
		return FALSE;
	if (!parse_string(sess_node, "SessionToken", &sess_tok))
		return FALSE;

	if (g_strcmp0(self->session_token, sess_tok) != 0) {
		g_free(self->session_token);
		self->session_token = g_strdup(sess_tok);
		g_object_notify(G_OBJECT(self), "session-token");
	}

	if (!parse_string(sess_node, "SessionId", &self->session_id))
		return FALSE;

	obj = json_node_get_object(sess_node);

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "profile_channel", &self->profile_channel) ||
	    !parse_string(node, "presence_channel", &self->presence_channel) ||
	    !parse_string(node, "id", &self->profile_id) ||
	    !parse_string(node, "display_name", &display_name))
		return FALSE;

	purple_connection_set_display_name(self->prpl_conn, display_name);

	node = json_object_get_member(obj, "Device");
	if (!parse_string(node, "DeviceId", &self->device_id) ||
	    !parse_string(node, "Channel", &self->device_channel))
		return FALSE;

	node = json_object_get_member(obj, "ServiceConfig");
	if (!node)
		return FALSE;
	obj = json_node_get_object(node);

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &self->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Push");
	if (!parse_string(node, "ReachabilityUrl", &self->reachability_url) ||
	    !parse_string(node, "WebsocketUrl", &self->websocket_url))
		return FALSE;

	node = json_object_get_member(obj, "Profile");
	if (!parse_string(node, "RestUrl", &self->profile_url))
		return FALSE;

	node = json_object_get_member(obj, "Contacts");
	if (!parse_string(node, "RestUrl", &self->contacts_url))
		return FALSE;

	node = json_object_get_member(obj, "Messaging");
	if (!parse_string(node, "RestUrl", &self->messaging_url))
		return FALSE;

	node = json_object_get_member(obj, "Presence");
	if (!parse_string(node, "RestUrl", &self->presence_url))
		return FALSE;

	node = json_object_get_member(obj, "Conference");
	if (!parse_string(node, "RestUrl", &self->conference_url))
		return FALSE;

	return TRUE;
}

static JsonNode *
chime_device_register_req(const gchar *devtoken)
{
	JsonBuilder *jb;
	JsonNode *jn;

	jb = json_builder_new();
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Device");
	jb = json_builder_begin_object(jb);
	jb = json_builder_set_member_name(jb, "Platform");
	jb = json_builder_add_string_value(jb, "osx");
	jb = json_builder_set_member_name(jb, "DeviceToken");
	jb = json_builder_add_string_value(jb, devtoken);
	jb = json_builder_set_member_name(jb, "Capabilities");
	jb = json_builder_add_int_value(jb, CHIME_DEVICE_CAP_PUSH_DELIVERY_RECEIPTS |
					CHIME_DEVICE_CAP_PRESENCE_PUSH |
					CHIME_DEVICE_CAP_PRESENCE_SUBSCRIPTION);
	jb = json_builder_end_object(jb);
	jb = json_builder_end_object(jb);
	jn = json_builder_get_root(jb);
	g_object_unref(jb);

	return jn;
}

static void register_cb(ChimeConnection *self, SoupMessage *msg,
			JsonNode *node, gpointer user_data)
{
	GTask *task = G_TASK(user_data);

	if (!node) {
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR, CHIME_CONNECTION_ERROR_NETWORK,
		                        _("Device registration failed"));
		g_object_unref(task);
		return;
	}

	self->reg_node = json_node_ref(node);
	if (!parse_regnode(self, self->reg_node)) {
		g_task_return_new_error(task, CHIME_CONNECTION_ERROR, CHIME_CONNECTION_ERROR_NETWORK,
		                        _("Failed to process registration response"));
		g_object_unref(task);
		return;
	}

	chime_init_juggernaut(self);

	chime_jugg_subscribe(self, self->profile_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, self->presence_channel, NULL, NULL, NULL);
	chime_jugg_subscribe(self, self->device_channel, NULL, NULL, NULL);

	chime_init_buddies(self);
	chime_init_rooms(self);
	chime_init_conversations(self);
	chime_init_chats(self);

	g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

void
chime_connection_register_device_async(ChimeConnection    *self,
                                       const gchar        *server,
                                       const gchar        *token,
                                       const gchar        *devtoken,
                                       GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));
	g_return_if_fail(server != NULL);
	g_return_if_fail(token != NULL);
	g_return_if_fail(devtoken != NULL);

	GTask *task = g_task_new(self, cancellable, callback, user_data);

	JsonNode *node = chime_device_register_req(devtoken);

	SoupURI *uri = soup_uri_new_printf(server, "/sessions");
	soup_uri_set_query_from_fields(uri, "Token", token, NULL);

	chime_connection_queue_http_request(self, node, uri, "POST", register_cb, task);
}

gboolean
chime_connection_register_device_finish(ChimeConnection  *self,
                                        GAsyncResult     *result,
                                        GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void set_status_cb(ChimeConnection *self, SoupMessage *msg,
                          JsonNode *node, gpointer user_data)
{
	g_task_return_boolean(G_TASK(user_data), TRUE);
}

void
chime_connection_set_status_async(ChimeConnection    *self,
                                  const gchar        *status,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data)
{
	g_return_if_fail(CHIME_IS_CONNECTION(self));

	GTask *task = g_task_new(self, cancellable, callback, user_data);
	JsonBuilder *builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "ManualAvailability");
	builder = json_builder_add_string_value(builder, status);
	builder = json_builder_end_object(builder);
	JsonNode *node = json_builder_get_root(builder);

	SoupURI *uri = soup_uri_new_printf(self->presence_url, "/presencesettings");
	chime_connection_queue_http_request(self, node, uri, "POST", set_status_cb, task);

	g_object_unref(builder);
}

gboolean
chime_connection_set_status_finish(ChimeConnection  *self,
                                   GAsyncResult     *result,
                                   GError          **error)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

const gchar *
chime_connection_get_session_token(ChimeConnection *self)
{
	g_return_val_if_fail(CHIME_IS_CONNECTION(self), NULL);

	return self->session_token;
}

/* If we get an auth failure on a standard request, we automatically attempt
 * to renew the authentication token and resubmit the request. */
static void renew_cb(ChimeConnection *self, SoupMessage *msg,
		     JsonNode *node, gpointer _unused)
{
	GList *l;
	const gchar *sess_tok;
	gchar *cookie_hdr;

	if (!node || !parse_string(node, "SessionToken", &sess_tok)) {
		purple_connection_error_reason(self->prpl_conn, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					       _("Failed to renew session token"));
		/* No need to cancel the outstanding requests; the session
		   will be torn down anyway. */
		return;
	}

	if (g_strcmp0(self->session_token, sess_tok) != 0) {
		g_free(self->session_token);
		self->session_token = g_strdup(sess_tok);
		g_object_notify(G_OBJECT(self), "session-token");
	}

	cookie_hdr = g_strdup_printf("_aws_wt_session=%s", self->session_token);

	while ( (l = g_list_first(self->msg_queue)) ) {
		struct chime_msg *cmsg = l->data;

		soup_message_headers_replace(cmsg->msg->request_headers, "Cookie", cookie_hdr);
		soup_session_queue_message(self->soup_sess, cmsg->msg, soup_msg_cb, cmsg);
		self->msg_queue = g_list_remove(self->msg_queue, cmsg);
	}

	g_free(cookie_hdr);
}

static void chime_renew_token(ChimeConnection *self)
{
	SoupURI *uri;
	JsonBuilder *builder;
	JsonNode *node;

	builder = json_builder_new();
	builder = json_builder_begin_object(builder);
	builder = json_builder_set_member_name(builder, "Token");
	builder = json_builder_add_string_value(builder, self->session_token);
	builder = json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	uri = soup_uri_new_printf(self->profile_url, "/tokens");
	soup_uri_set_query_from_fields(uri, "Token", self->session_token, NULL);
	chime_connection_queue_http_request(self, node, uri, "POST", renew_cb, NULL);

	g_object_unref(builder);
}

/* First callback for SoupMessage completion — do the common
 * parsing of the JSON response (if any) and hand it on to the
 * real callback function. Also handles auth token renewal. */
static void soup_msg_cb(SoupSession *soup_sess, SoupMessage *msg, gpointer _cmsg)
{
	struct chime_msg *cmsg = _cmsg;
	ChimeConnection *cxn = cmsg->cxn;
	JsonParser *parser = NULL;
	JsonNode *node = NULL;

	/* Special case for renew_cb itself, which mustn't recurse! */
	if ((cmsg->cb != renew_cb) && msg->status_code == 401) {
		g_object_ref(msg);
		if (cxn->msg_queue) {
			/* Already renewing; just add to the list */
			cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
			return;
		}

		cxn->msg_queue = g_list_append(cxn->msg_queue, cmsg);
		chime_renew_token(cxn);
		return;
	}

	const gchar *content_type = soup_message_headers_get_content_type(msg->response_headers, NULL);
	if (content_type && !strcmp(content_type, "application/json")) {
		GError *error = NULL;

		parser = json_parser_new();
		if (!json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, &error)) {
			g_warning("Error loading data: %s", error->message);
			g_error_free(error);
		} else {
			node = json_parser_get_root(parser);
		}
	}

	if (cmsg->cb)
		cmsg->cb(cmsg->cxn, msg, node, cmsg->cb_data);
	g_clear_object(&parser);
	g_free(cmsg);
}

SoupMessage *
chime_connection_queue_http_request(ChimeConnection *self, JsonNode *node,
				    SoupURI *uri, const gchar *method,
				    ChimeSoupMessageCallback callback,
				    gpointer cb_data)
{
	struct chime_msg *cmsg = g_new0(struct chime_msg, 1);

	cmsg->cxn = self;
	cmsg->cb = callback;
	cmsg->cb_data = cb_data;
	cmsg->msg = soup_message_new_from_uri(method, uri);
	soup_uri_free(uri);

	if (self->session_token) {
		gchar *cookie = g_strdup_printf("_aws_wt_session=%s", self->session_token);
		soup_message_headers_append(cmsg->msg->request_headers, "Cookie", cookie);
		g_free(cookie);
	}

	soup_message_headers_append(cmsg->msg->request_headers, "Accept", "*/*");
	soup_message_headers_append(cmsg->msg->request_headers, "User-Agent", "Pidgin-Chime " PACKAGE_VERSION);
	if (node) {
		gchar *body;
		gsize body_size;
		JsonGenerator *gen = json_generator_new();
		json_generator_set_root(gen, node);
		body = json_generator_to_data(gen, &body_size);
		soup_message_set_request(cmsg->msg, "application/json",
					 SOUP_MEMORY_TAKE,
					 body, body_size);
		g_object_unref(gen);
		json_node_unref(node);
	}

	soup_session_queue_message(self->soup_sess, cmsg->msg, soup_msg_cb, cmsg);

	return cmsg->msg;
}