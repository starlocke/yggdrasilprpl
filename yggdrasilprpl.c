/**
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with its
 * source distribution.
 *
 * -----------------------------------------------------------------------------
 *
 * Yggdrasilprpl is a protocol plugin for Pidgin and libpurple. You can create
 * exactly one account to interface with YggdrasilRadio's webservices.
 *
 * The only feature currently supported is intercom/chat.
 *
 * FIXME: Paths are currently hardcoded for linux-like operating systems.
 *
 * TODO: I have no idea how to create "Makefiles". I compiled this by hacking
 *       the upstream makefile mechanisms, adding "yggdrasil" into the list of
 *       supported protocols, then running ./configure
 *
 * Yggdrasilprpl is based off the excellent "null protocol" skeleton found in
 * the original pidgin/libpurple source tree.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>
#include <signal.h>

#include <curl/curl.h>
#include <glib.h>

/* If you're using this as the basis of a prpl that will be distributed
 * separately from libpurple, remove the internal.h include below and replace
 * it with code to include your own config.h or similar.  If you're going to
 * provide for translation, you'll also need to setup the gettext macros. */
#include "internal.h"

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "cmds.h"
#include "conversation.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "privacy.h"
#include "prpl.h"
#include "roomlist.h"
#include "status.h"
#include "util.h"
#include "version.h"


#define YGGDRASILPRPL_ID "prpl-yggdrasil"
static PurplePlugin *_yggdrasil_protocol = NULL;

#define YGGDRASIL_STATUS_ONLINE   "online"
#define YGGDRASIL_STATUS_AWAY     "away"
#define YGGDRASIL_STATUS_OFFLINE  "offline"
#define YGGDRASIL_NETWORK         "yggdrasilradio.net"
#define PLUGIN_DEBUG_NAME    "yggdrasilprpl"
#define YGGDRASIL_DATA_AUTH  "/tmp/yggdrasil.auth.txt"
#define YGGDRASIL_DATA_CHAT_LOG  "/tmp/yggdrasil.chat.log.txt"
#define YGGDRASIL_DATA_CHAT_NEW  "/tmp/yggdrasil.chat.new.txt"
#define YGGDRASIL_DATA_CHAT_TMP  "/tmp/yggdrasil.chat.tmp.txt"
#define YGGDRASIL_DATA_TOPIC  "/tmp/yggdrasil.topic.txt"
#define YGGDRASIL_DATA_USERS  "/tmp/yggdrasil.users.txt"

#define YGGDRASIL_REFRESH_CHAT_INTERVAL   10

#define CURL_MAX_BUF	65536

char wr_buf[CURL_MAX_BUF+1];
int  wr_index;

typedef void (*GcFunc)(PurpleConnection *from,
                       PurpleConnection *to,
                       gpointer userdata);

typedef struct {
  GcFunc fn;
  PurpleConnection *from;
  gpointer userdata;
} GcFuncData;

char from_hex(char ch);
char to_hex(char code);
char *url_encode(const char *str);

char AUTH_CHAT[80];
char AUTH_SEARCH[80];
char AUTH_SEARCH_SUBDOMAIN[80];

/*
 * stores offline messages that haven't been delivered yet. maps username
 * (char *) to GList * of GOfflineMessages. initialized in yggdrasilprpl_init.
 */
GHashTable* goffline_messages = NULL;

typedef struct {
  char *from;
  char *message;
  time_t mtime;
  PurpleMessageFlags flags;
} GOfflineMessage;

/*
 * helpers
 */
static PurpleConnection *get_yggdrasilprpl_gc(const char *username) {
  PurpleAccount *acct = purple_accounts_find(username, YGGDRASILPRPL_ID);
  if (acct && purple_account_is_connected(acct))
    return acct->gc;
  else
    return NULL;
}

static void call_if_yggdrasilprpl(gpointer data, gpointer userdata) {
  PurpleConnection *gc = (PurpleConnection *)(data);
  GcFuncData *gcfdata = (GcFuncData *)userdata;

  if (!strcmp(gc->account->protocol_id, YGGDRASILPRPL_ID))
    gcfdata->fn(gcfdata->from, gc, gcfdata->userdata);
}

static void foreach_yggdrasilprpl_gc(GcFunc fn, PurpleConnection *from,
                                gpointer userdata) {
  GcFuncData gcfdata = { fn, from, userdata };
  g_list_foreach(purple_connections_get_all(), call_if_yggdrasilprpl,
                 &gcfdata);
}


typedef void(*ChatFunc)(PurpleConvChat *from, PurpleConvChat *to,
                        int id, const char *room, gpointer userdata);

typedef struct {
  ChatFunc fn;
  PurpleConvChat *from_chat;
  gpointer userdata;
} ChatFuncData;

static void call_chat_func(gpointer data, gpointer userdata) {
  PurpleConnection *to = (PurpleConnection *)data;
  ChatFuncData *cfdata = (ChatFuncData *)userdata;

  int id = cfdata->from_chat->id;
  PurpleConversation *conv = purple_find_chat(to, id);
  if (conv) {
    PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
    cfdata->fn(cfdata->from_chat, chat, id, conv->name, cfdata->userdata);
  }
}

static void foreach_gc_in_chat(ChatFunc fn, PurpleConnection *from,
                               int id, gpointer userdata) {
  PurpleConversation *conv = purple_find_chat(from, id);
  ChatFuncData cfdata = { fn,
                          purple_conversation_get_chat_data(conv),
                          userdata };
  g_list_foreach(purple_connections_get_all(), call_chat_func,
                 &cfdata);
}

static PurpleConversation *current_conv;
static PurpleConvChat *current_chat;
static void yggdrasilprpl_chat_update_convo(PurpleConvChat *chat);
static void yggdrasilprpl_chat_update_topic(PurpleConvChat *chat);
static void yggdrasilprpl_chat_update_users(PurpleConvChat *chat, const char *account_username);
static void chatread();

static void refresh(int signal_code){
  signal(SIGALRM, SIG_IGN);
  chatread();
  yggdrasilprpl_chat_update_topic(current_chat);
  yggdrasilprpl_chat_update_users(current_chat, "");
  yggdrasilprpl_chat_update_convo(current_chat);
  signal(SIGALRM, refresh);
  alarm(YGGDRASIL_REFRESH_CHAT_INTERVAL);
}

static void discover_status(PurpleConnection *from, PurpleConnection *to,
                            gpointer userdata) {
  const char *from_username = from->account->username;
  const char *to_username = to->account->username;

  if (purple_find_buddy(from->account, to_username)) {
    PurpleStatus *status = purple_account_get_active_status(to->account);
    const char *status_id = purple_status_get_id(status);
    const char *message = purple_status_get_attr_string(status, "message");

    if (!strcmp(status_id, YGGDRASIL_STATUS_ONLINE) ||
        !strcmp(status_id, YGGDRASIL_STATUS_AWAY) ||
        !strcmp(status_id, YGGDRASIL_STATUS_OFFLINE)) {
      purple_debug_info(PLUGIN_DEBUG_NAME, "%s sees that %s is %s: %s\n",
                        from_username, to_username, status_id, message);
      purple_prpl_got_user_status(from->account, to_username, status_id,
                                  (message) ? "message" : NULL, message, NULL);
    } else {
      purple_debug_error(PLUGIN_DEBUG_NAME,
                         "%s's buddy %s has an unknown status: %s, %s",
                         from_username, to_username, status_id, message);
    }
  }
}

static void report_status_change(PurpleConnection *from, PurpleConnection *to,
                                 gpointer userdata) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "notifying %s that %s changed status\n",
                    to->account->username, from->account->username);
  discover_status(to, from, NULL);
}

static void chatread(void);
static void chatread(){
  int ret;
  ret = system("curl --silent http://yggdrasilradio.net/chatread.php?n=15 | head -n -1 | tail -n +2 | sed -e 's#<br>##g' -e 's#&nbsp;# #g' > /tmp/yggdrasil.chat.tmp.txt");
  if(ret) printf("problem?");
  ret = system("curl --silent http://yggdrasilradio.net/chatread.php?n=0 | cut -d '|' -f 2 > /tmp/yggdrasil.topic.txt");
  if(ret) printf("problem?");
  ret = system("curl --silent http://yggdrasilradio.net/chatread.php?n=0 | cut -d '|' -f 4 | sed -e 's#</span>, #</span>\\n#g' -e 's#</span><span#</span>\\n<span#g' | head -n -1 | sed 's#<span.*title=\"\\(.*\\)\">\\(.*\\)</span>#\\2 @ \\1#g' > /tmp/yggdrasil.users.txt");
  if(ret) printf("problem?");
}

/*
 * UI callbacks
 */
static void yggdrasilprpl_input_user_info(PurplePluginAction *action)
{
  PurpleConnection *gc = (PurpleConnection *)action->context;
  PurpleAccount *acct = purple_connection_get_account(gc);
  purple_debug_info(PLUGIN_DEBUG_NAME, "showing 'Set User Info' dialog for %s\n",
                    acct->username);

  purple_account_request_change_user_info(acct);
}

/* this is set to the actions member of the PurplePluginInfo struct at the
 * bottom.
 */
static GList *yggdrasilprpl_actions(PurplePlugin *plugin, gpointer context)
{
  PurplePluginAction *action = purple_plugin_action_new(
    _("Set User Info..."), yggdrasilprpl_input_user_info);
  return g_list_append(NULL, action);
}


/*
 * prpl functions
 */
static const char *yggdrasilprpl_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
  return "yggdrasil";
}

static char *yggdrasilprpl_status_text(PurpleBuddy *buddy) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "getting %s's status text for %s\n",
                    buddy->name, buddy->account->username);

  if (purple_find_buddy(buddy->account, buddy->name)) {
    PurplePresence *presence = purple_buddy_get_presence(buddy);
    PurpleStatus *status = purple_presence_get_active_status(presence);
    const char *name = purple_status_get_name(status);
    const char *message = purple_status_get_attr_string(status, "message");

    char *text;
    if (message && strlen(message) > 0)
      text = g_strdup_printf("%s: %s", name, message);
    else
      text = g_strdup(name);

    purple_debug_info(PLUGIN_DEBUG_NAME, "%s's status text is %s\n", buddy->name, text);
    return text;

  } else {
    purple_debug_info(PLUGIN_DEBUG_NAME, "...but %s is not logged in\n", buddy->name);
    return g_strdup("Not logged in");
  }
}

static void yggdrasilprpl_tooltip_text(PurpleBuddy *buddy,
                                  PurpleNotifyUserInfo *info,
                                  gboolean full) {
  PurpleConnection *gc = get_yggdrasilprpl_gc(buddy->name);

  if (gc) {
    /* they're logged in */
    PurplePresence *presence = purple_buddy_get_presence(buddy);
    PurpleStatus *status = purple_presence_get_active_status(presence);
    char *msg = yggdrasilprpl_status_text(buddy);
    purple_notify_user_info_add_pair(info, purple_status_get_name(status),
                                     msg);
    g_free(msg);

    if (full) {
      const char *user_info = purple_account_get_user_info(gc->account);
      if (user_info)
        purple_notify_user_info_add_pair(info, _("User info"), user_info);
    }

  } else {
    /* they're not logged in */
    purple_notify_user_info_add_pair(info, _("User info"), _("not logged in"));
  }

  purple_debug_info(PLUGIN_DEBUG_NAME, "showing %s tooltip for %s\n",
                    (full) ? "full" : "short", buddy->name);
}

static GList *yggdrasilprpl_status_types(PurpleAccount *acct)
{
  GList *types = NULL;
  PurpleStatusType *type;

  purple_debug_info(PLUGIN_DEBUG_NAME, "returning status types for %s: %s, %s, %s\n",
                    acct->username,
                    YGGDRASIL_STATUS_ONLINE, YGGDRASIL_STATUS_AWAY, YGGDRASIL_STATUS_OFFLINE);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_AVAILABLE,
      YGGDRASIL_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_AWAY,
      YGGDRASIL_STATUS_AWAY, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_OFFLINE,
      YGGDRASIL_STATUS_OFFLINE, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  return g_list_reverse(types);
}

static void blist_example_menu_item(PurpleBlistNode *node, gpointer userdata) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "example menu item clicked on user %s\n",
                    ((PurpleBuddy *)node)->name);

  purple_notify_info(NULL,  /* plugin handle or PurpleConnection */
                     _("Primary title"),
                     _("Secondary title"),
                     _("This is the callback for the yggdrasilprpl menu item."));
}

static GList *yggdrasilprpl_blist_node_menu(PurpleBlistNode *node) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "providing buddy list context menu item\n");

  if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
    PurpleMenuAction *action = purple_menu_action_new(
      _("Yggdrasilprpl example menu item"),
      PURPLE_CALLBACK(blist_example_menu_item),
      NULL,   /* userdata passed to the callback */
      NULL);  /* child menu items */
    return g_list_append(NULL, action);
  } else {
    return NULL;
  }
}

static GList *yggdrasilprpl_chat_info(PurpleConnection *gc) {
  struct proto_chat_entry *pce; /* defined in prpl.h */

  purple_debug_info(PLUGIN_DEBUG_NAME, "returning chat setting 'room'\n");

  pce = g_new0(struct proto_chat_entry, 1);
  pce->label = _("Chat _room");
  pce->identifier = "room";
  pce->required = TRUE;

  return g_list_append(NULL, pce);
}

static GHashTable *yggdrasilprpl_chat_info_defaults(PurpleConnection *gc,
                                               const char *room) {
  GHashTable *defaults;

  purple_debug_info(PLUGIN_DEBUG_NAME, "returning chat default setting "
                    "'room' = 'default'\n");

  defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  g_hash_table_insert(defaults, "room", g_strdup("default"));
  return defaults;
}

static void yggdrasilprpl_chat_update_users(PurpleConvChat *chat, const char *account_username){
  char user[80];
  FILE *fp;
  fp = fopen(YGGDRASIL_DATA_USERS, "rt");
  purple_conv_chat_clear_users(chat);
  while(fgets(user, 80, fp) != NULL){
    user[strlen(user)-1] = 0;
    if( strcmp(user, account_username) != 0 ){
      purple_conv_chat_add_user(chat, user, "", PURPLE_CBFLAGS_NONE, FALSE);
    }
  }
  fclose(fp);
}

static void yggdrasilprpl_chat_update_topic(PurpleConvChat *chat){
  char topic[4096];
  FILE *fp;
  fp = fopen(YGGDRASIL_DATA_TOPIC, "rt");
  if(fgets(topic, 4096, fp) != NULL){
    topic[strlen(topic)-1] = 0;
    purple_conv_chat_set_topic(chat, "system", topic);
  }
  fclose(fp);
}

static void yggdrasilprpl_chat_update_convo(PurpleConvChat *chat){
  char message[4096];
  int ret;
  FILE *fp;

  ret = system("diff /tmp/yggdrasil.chat.log.txt /tmp/yggdrasil.chat.tmp.txt | tail -n +2 | grep '^>' | sed 's/^> //g' > /tmp/yggdrasil.chat.new.txt");
  fp = fopen(YGGDRASIL_DATA_CHAT_NEW, "rt");
  while(fgets(message, 4096, fp) != NULL){
    message[strlen(message)-1] = 0;
    if(strlen(message) > 0){
      purple_conv_chat_write(chat, "?", message, PURPLE_MESSAGE_RAW | PURPLE_MESSAGE_NO_LOG | PURPLE_MESSAGE_RECV, time(NULL));
    }
  }
  fclose(fp);

  ret = system("cat /tmp/yggdrasil.chat.new.txt >> /tmp/yggdrasil.chat.log.txt");
  ret = system("tail -n 99 /tmp/yggdrasil.chat.log.txt > /tmp/yggdrasil.chat.tmp.txt");
  ret = system("cat /tmp/yggdrasil.chat.tmp.txt > /tmp/yggdrasil.chat.log.txt");

  if(ret) printf("problem?"); // I don't really care right now.
}

static void yggdrasilprpl_login(PurpleAccount *acct)
{
  PurpleConnection *gc = purple_account_get_connection(acct);
  GList *offline_messages;
  const char *password;
  char *login_cmd;
  char *escaped_username;
  char *escaped_password;
  int ret;
  PurpleChat* pchat;
  GHashTable *pchat_components;
  char line[80];
  FILE *fp;
  int auth_line = 1;

  purple_debug_info(PLUGIN_DEBUG_NAME, "logging in %s\n", acct->username);

  purple_connection_update_progress(gc, _("Connecting"),
                                    0,   /* which connection step this is */
                                    2);  /* total number of steps */

  ret = system("echo '' > /tmp/yggdrasil.auth.txt");
  password = purple_account_get_password(acct);
  escaped_username = url_encode(acct->username);
  escaped_password = url_encode(password);
  login_cmd = g_strdup_printf(
    "curl --silent \"http://yggdrasilradio.net/login.php?uid=%s&pwd=%s\" | sed -e 's/$/\\n/' -e 's/|/\\n/g' > /tmp/yggdrasil.auth.txt"
    , escaped_username
    , escaped_password
  );
  ret = system(login_cmd);
  if(ret) printf("problem?\n"); // I don't really care :P

  fp = fopen(YGGDRASIL_DATA_AUTH, "rt");
  while(fgets(line, 80, fp) != NULL){
    line[strlen(line)-1] = 0;
    if(strlen(line) > 0){
      if(auth_line == 1){
        strcpy( AUTH_CHAT, line );
        auth_line = 2;
      }
      else if(auth_line == 2){
        strcpy( AUTH_SEARCH, line );
        auth_line = 3;
      }
      else if(auth_line == 3){
        strcpy( AUTH_SEARCH_SUBDOMAIN, line );
        auth_line = -1;
      }
    }
  }
  fclose(fp);

  purple_connection_update_progress(gc, _("Connected"),
                                    1,   /* which connection step this is */
                                    2);  /* total number of steps */
  purple_connection_set_state(gc, PURPLE_CONNECTED);

  pchat = purple_blist_find_chat(acct, "Yggdrasil Intercom");
  if(pchat != NULL){
  }
  else {
    pchat_components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_replace(pchat_components, "room", "Yggdrasil Intercom");
    pchat = purple_chat_new(acct, "", pchat_components);
    purple_blist_add_chat(pchat, NULL, NULL);
  }
  /* tell purple about everyone on our buddy list who's connected */
  foreach_yggdrasilprpl_gc(discover_status, gc, NULL);

  /* notify other yggdrasilprpl accounts */
  foreach_yggdrasilprpl_gc(report_status_change, gc, NULL);

  /* fetch stored offline messages */
  purple_debug_info(PLUGIN_DEBUG_NAME, "checking for offline messages for %s\n",
                    acct->username);
  offline_messages = g_hash_table_lookup(goffline_messages, acct->username);
  while (offline_messages) {
    GOfflineMessage *message = (GOfflineMessage *)offline_messages->data;
    purple_debug_info(PLUGIN_DEBUG_NAME, "delivering offline message to %s: %s\n",
                      acct->username, message->message);
    serv_got_im(gc, message->from, message->message, message->flags,
                message->mtime);
    offline_messages = g_list_next(offline_messages);

    g_free(message->from);
    g_free(message->message);
    g_free(message);
  }

  g_list_free(offline_messages);
  g_hash_table_remove(goffline_messages, &acct->username);
}

static void yggdrasilprpl_close(PurpleConnection *gc)
{
  /* notify other yggdrasilprpl accounts */
  foreach_yggdrasilprpl_gc(report_status_change, gc, NULL);
}

static int yggdrasilprpl_send_im(PurpleConnection *gc, const char *who,
                            const char *message, PurpleMessageFlags flags)
{
  const char *from_username = gc->account->username;
  PurpleMessageFlags receive_flags = ((flags & ~PURPLE_MESSAGE_SEND)
                                      | PURPLE_MESSAGE_RECV);
  PurpleAccount *to_acct = purple_accounts_find(who, YGGDRASILPRPL_ID);
  PurpleConnection *to;

  purple_debug_info(PLUGIN_DEBUG_NAME, "sending message from %s to %s: %s\n",
                    from_username, who, message);

  /* is the sender blocked by the recipient's privacy settings? */
  if (to_acct && !purple_privacy_check(to_acct, gc->account->username)) {
    char *msg = g_strdup_printf(
      _("Your message was blocked by %s's privacy settings."), who);
    purple_debug_info(PLUGIN_DEBUG_NAME,
                      "discarding; %s is blocked by %s's privacy settings\n",
                      from_username, who);
    purple_conv_present_error(who, gc->account, msg);
    g_free(msg);
    return 0;
  }

  /* is the recipient online? */
  to = get_yggdrasilprpl_gc(who);
  if (to) {  /* yes, send */
    serv_got_im(to, from_username, message, receive_flags, time(NULL));

  } else {  /* nope, store as an offline message */
    GOfflineMessage *offline_message;
    GList *messages;

    purple_debug_info(PLUGIN_DEBUG_NAME,
                      "%s is offline, sending as offline message\n", who);
    offline_message = g_new0(GOfflineMessage, 1);
    offline_message->from = g_strdup(from_username);
    offline_message->message = g_strdup(message);
    offline_message->mtime = time(NULL);
    offline_message->flags = receive_flags;

    messages = g_hash_table_lookup(goffline_messages, who);
    messages = g_list_append(messages, offline_message);
    g_hash_table_insert(goffline_messages, g_strdup(who), messages);
  }

   return 1;
}

static void yggdrasilprpl_set_info(PurpleConnection *gc, const char *info) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "setting %s's user info to %s\n",
                    gc->account->username, info);
}

static const char *typing_state_to_string(PurpleTypingState typing) {
  switch (typing) {
  case PURPLE_NOT_TYPING:  return "is not typing";
  case PURPLE_TYPING:      return "is typing";
  case PURPLE_TYPED:       return "stopped typing momentarily";
  default:               return "unknown typing state";
  }
}

static void notify_typing(PurpleConnection *from, PurpleConnection *to,
                          gpointer typing) {
  const char *from_username = from->account->username;
  const char *action = typing_state_to_string((PurpleTypingState)typing);
  purple_debug_info(PLUGIN_DEBUG_NAME, "notifying %s that %s %s\n",
                    to->account->username, from_username, action);

  serv_got_typing(to,
                  from_username,
                  0, /* if non-zero, a timeout in seconds after which to
                      * reset the typing status to PURPLE_NOT_TYPING */
                  (PurpleTypingState)typing);
}

/* Converts a hex character to its integer value */
char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_encode(const char *str) {
  const char *pstr = str;
  char *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
    if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~')
      *pbuf++ = *pstr;
    else if (*pstr == ' ')
      *pbuf++ = '+';
    else
      *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
    pstr++;
  }
  *pbuf = '\0';
  return buf;
}

static unsigned int yggdrasilprpl_send_typing(PurpleConnection *gc, const char *name,
                                         PurpleTypingState typing) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s %s\n", gc->account->username,
                    typing_state_to_string(typing));
  foreach_yggdrasilprpl_gc(notify_typing, gc, (gpointer)typing);
  return 0;
}

static void yggdrasilprpl_get_info(PurpleConnection *gc, const char *username) {
  const char *body;
  PurpleNotifyUserInfo *info = purple_notify_user_info_new();
  PurpleAccount *acct;

  purple_debug_info(PLUGIN_DEBUG_NAME, "Fetching %s's user info for %s\n", username,
                    gc->account->username);

  if (!get_yggdrasilprpl_gc(username)) {
    char *msg = g_strdup_printf(_("%s is not logged in."), username);
    purple_notify_error(gc, _("User Info"), _("User info not available. "), msg);
    g_free(msg);
  }

  acct = purple_accounts_find(username, YGGDRASILPRPL_ID);
  if (acct)
    body = purple_account_get_user_info(acct);
  else
    body = _("No user info.");
  purple_notify_user_info_add_pair(info, "Info", body);

  /* show a buddy's user info in a nice dialog box */
  purple_notify_userinfo(gc,        /* connection the buddy info came through */
                         username,  /* buddy's username */
                         info,      /* body */
                         NULL,      /* callback called when dialog closed */
                         NULL);     /* userdata for callback */
}

static void yggdrasilprpl_set_status(PurpleAccount *acct, PurpleStatus *status) {
  const char *msg = purple_status_get_attr_string(status, "message");
  purple_debug_info(PLUGIN_DEBUG_NAME, "setting %s's status to %s: %s\n",
                    acct->username, purple_status_get_name(status), msg);

  foreach_yggdrasilprpl_gc(report_status_change, get_yggdrasilprpl_gc(acct->username),
                      NULL);
}

static void yggdrasilprpl_set_idle(PurpleConnection *gc, int idletime) {
  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "purple reports that %s has been idle for %d seconds\n",
                    gc->account->username, idletime);
}

static void yggdrasilprpl_change_passwd(PurpleConnection *gc, const char *old_pass,
                                   const char *new_pass) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s wants to change their password\n",
                    gc->account->username);
}

static void yggdrasilprpl_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                               PurpleGroup *group)
{
  const char *username = gc->account->username;
  PurpleConnection *buddy_gc = get_yggdrasilprpl_gc(buddy->name);

  purple_debug_info(PLUGIN_DEBUG_NAME, "adding %s to %s's buddy list\n", buddy->name,
                    username);

  if (buddy_gc) {
    PurpleAccount *buddy_acct = buddy_gc->account;

    discover_status(gc, buddy_gc, NULL);

    if (purple_find_buddy(buddy_acct, username)) {
      purple_debug_info(PLUGIN_DEBUG_NAME, "%s is already on %s's buddy list\n",
                        username, buddy->name);
    } else {
      purple_debug_info(PLUGIN_DEBUG_NAME, "asking %s if they want to add %s\n",
                        buddy->name, username);
      purple_account_request_add(buddy_acct,
                                 username,
                                 NULL,   /* local account id (rarely used) */
                                 NULL,   /* alias */
                                 NULL);  /* message */
    }
  }
}

static void yggdrasilprpl_add_buddies(PurpleConnection *gc, GList *buddies,
                                 GList *groups) {
  GList *buddy = buddies;
  GList *group = groups;

  purple_debug_info(PLUGIN_DEBUG_NAME, "adding multiple buddies\n");

  while (buddy && group) {
    yggdrasilprpl_add_buddy(gc, (PurpleBuddy *)buddy->data, (PurpleGroup *)group->data);
    buddy = g_list_next(buddy);
    group = g_list_next(group);
  }
}

static void yggdrasilprpl_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                                  PurpleGroup *group)
{
  purple_debug_info(PLUGIN_DEBUG_NAME, "removing %s from %s's buddy list\n",
                    buddy->name, gc->account->username);
}

static void yggdrasilprpl_remove_buddies(PurpleConnection *gc, GList *buddies,
                                    GList *groups) {
  GList *buddy = buddies;
  GList *group = groups;

  purple_debug_info(PLUGIN_DEBUG_NAME, "removing multiple buddies\n");

  while (buddy && group) {
    yggdrasilprpl_remove_buddy(gc, (PurpleBuddy *)buddy->data,
                          (PurpleGroup *)group->data);
    buddy = g_list_next(buddy);
    group = g_list_next(group);
  }
}

/*
 * yggdrasilprpl uses purple's local whitelist and blacklist, stored in blist.xml, as
 * its authoritative privacy settings, and uses purple's logic (specifically
 * purple_privacy_check(), from privacy.h), to determine whether messages are
 * allowed or blocked.
 */
static void yggdrasilprpl_add_permit(PurpleConnection *gc, const char *name) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s adds %s to their allowed list\n",
                    gc->account->username, name);
}

static void yggdrasilprpl_add_deny(PurpleConnection *gc, const char *name) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s adds %s to their blocked list\n",
                    gc->account->username, name);
}

static void yggdrasilprpl_rem_permit(PurpleConnection *gc, const char *name) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s removes %s from their allowed list\n",
                    gc->account->username, name);
}

static void yggdrasilprpl_rem_deny(PurpleConnection *gc, const char *name) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s removes %s from their blocked list\n",
                    gc->account->username, name);
}

static void yggdrasilprpl_set_permit_deny(PurpleConnection *gc) {
  /* this is for synchronizing the local black/whitelist with the server.
   * for yggdrasilprpl, it's a noop.
   */
}

static void joined_chat(PurpleConvChat *from, PurpleConvChat *to,
                        int id, const char *room, gpointer userdata) {
  /*  tell their chat window that we joined */
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s sees that %s joined chat room %s\n",
                    to->nick, from->nick, room);
  purple_conv_chat_add_user(to,
                            from->nick,
                            NULL,   /* user-provided join message, IRC style */
                            PURPLE_CBFLAGS_NONE,
                            TRUE);  /* show a join message */

  if (from != to) {
    /* add them to our chat window */
    purple_debug_info(PLUGIN_DEBUG_NAME, "%s sees that %s is in chat room %s\n",
                      from->nick, to->nick, room);

    purple_conv_chat_add_user(from,
                              to->nick,
                              NULL,   /* user-provided join message, IRC style */
                              PURPLE_CBFLAGS_NONE,
                              FALSE);  /* show a join message */
  }
}

static void yggdrasilprpl_join_chat(PurpleConnection *gc, GHashTable *components) {
  PurpleConversation *conv;
  PurpleConvChat* chat;
  const char *username = gc->account->username;
  const char *room = g_hash_table_lookup(components, "room");
  int chat_id = g_str_hash(room);
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s is joining chat room %s\n", username, room);

  conv = purple_find_chat(gc, chat_id);
  if (!conv) {
    serv_got_joined_chat(gc, chat_id, room);

    /* tell everyone that we joined, and add them if they're already there */
    foreach_gc_in_chat(joined_chat, gc, chat_id, NULL);

    conv = purple_find_chat(gc, chat_id);
  } else {
    purple_debug_info(PLUGIN_DEBUG_NAME, "%s is already in chat room %s\n", username,
                      room);
  }
  chatread(); // Update from website.

  chat = purple_conversation_get_chat_data(conv);
  yggdrasilprpl_chat_update_users(chat, username);
  yggdrasilprpl_chat_update_topic(chat);
  yggdrasilprpl_chat_update_convo(chat);

  current_conv = conv;
  current_chat = chat;
  signal(SIGALRM, SIG_IGN);
  signal(SIGALRM, refresh);
  alarm(YGGDRASIL_REFRESH_CHAT_INTERVAL);
}

static void yggdrasilprpl_reject_chat(PurpleConnection *gc, GHashTable *components) {
  const char *invited_by = g_hash_table_lookup(components, "invited_by");
  const char *room = g_hash_table_lookup(components, "room");
  const char *username = gc->account->username;
  PurpleConnection *invited_by_gc = get_yggdrasilprpl_gc(invited_by);
  char *message = g_strdup_printf(
    "%s %s %s.",
    username,
    _("has rejected your invitation to join the chat room"),
    room);

  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "%s has rejected %s's invitation to join chat room %s\n",
                    username, invited_by, room);

  purple_notify_info(invited_by_gc,
                     _("Chat invitation rejected"),
                     _("Chat invitation rejected"),
                     message);
  g_free(message);
}

static char *yggdrasilprpl_get_chat_name(GHashTable *components) {
  const char *room = g_hash_table_lookup(components, "room");
  purple_debug_info(PLUGIN_DEBUG_NAME, "reporting chat room name '%s'\n", room);
  return g_strdup(room);
}

static void yggdrasilprpl_chat_invite(PurpleConnection *gc, int id,
                                 const char *message, const char *who) {
  const char *username = gc->account->username;
  PurpleConversation *conv = purple_find_chat(gc, id);
  const char *room = conv->name;
  PurpleAccount *to_acct = purple_accounts_find(who, YGGDRASILPRPL_ID);

  purple_debug_info(PLUGIN_DEBUG_NAME, "%s is inviting %s to join chat room %s\n",
                    username, who, room);

  if (to_acct) {
    PurpleConversation *to_conv = purple_find_chat(to_acct->gc, id);
    if (to_conv) {
      char *tmp = g_strdup_printf("%s is already in chat room %s.", who, room);
      purple_debug_info(PLUGIN_DEBUG_NAME,
                        "%s is already in chat room %s; "
                        "ignoring invitation from %s\n",
                        who, room, username);
      purple_notify_info(gc, _("Chat invitation"), _("Chat invitation"), tmp);
      g_free(tmp);
    } else {
      GHashTable *components;
      components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
      g_hash_table_replace(components, "room", g_strdup(room));
      g_hash_table_replace(components, "invited_by", g_strdup(username));
      serv_got_chat_invite(to_acct->gc, room, username, message, components);
    }
  }
}

static void left_chat_room(PurpleConvChat *from, PurpleConvChat *to,
                           int id, const char *room, gpointer userdata) {
  if (from != to) {
    /*  tell their chat window that we left */
    purple_debug_info(PLUGIN_DEBUG_NAME, "%s sees that %s left chat room %s\n",
                      to->nick, from->nick, room);
    purple_conv_chat_remove_user(to,
                                 from->nick,
                                 NULL);  /* user-provided message, IRC style */
  }
}

static void yggdrasilprpl_chat_leave(PurpleConnection *gc, int id) {
  PurpleConversation *conv = purple_find_chat(gc, id);
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s is leaving chat room %s\n",
                    gc->account->username, conv->name);

  /* tell everyone that we left */
  foreach_gc_in_chat(left_chat_room, gc, id, NULL);
}

static PurpleCmdRet send_whisper(PurpleConversation *conv, const gchar *cmd,
                                 gchar **args, gchar **error, void *userdata) {
  const char *to_username;
  const char *message;
  const char *from_username;
  PurpleConvChat *chat;
  PurpleConvChatBuddy *chat_buddy;
  PurpleConnection *to;

  /* parse args */
  to_username = args[0];
  message = args[1];

  if (!to_username || strlen(to_username) == 0) {
    *error = g_strdup(_("Whisper is missing recipient."));
    return PURPLE_CMD_RET_FAILED;
  } else if (!message || strlen(message) == 0) {
    *error = g_strdup(_("Whisper is missing message."));
    return PURPLE_CMD_RET_FAILED;
  }

  from_username = conv->account->username;
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s whispers to %s in chat room %s: %s\n",
                    from_username, to_username, conv->name, message);

  chat = purple_conversation_get_chat_data(conv);
  chat_buddy = purple_conv_chat_cb_find(chat, to_username);
  to = get_yggdrasilprpl_gc(to_username);

  if (!chat_buddy) {
    /* this will be freed by the caller */
    *error = g_strdup_printf(_("%s is not logged in."), to_username);
    return PURPLE_CMD_RET_FAILED;
  } else if (!to) {
    *error = g_strdup_printf(_("%s is not in this chat room."), to_username);
    return PURPLE_CMD_RET_FAILED;
  } else {
    /* write the whisper in the sender's chat window  */
    char *message_to = g_strdup_printf("%s (to %s)", message, to_username);
    purple_conv_chat_write(chat, from_username, message_to,
                           PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_WHISPER,
                           time(NULL));
    g_free(message_to);

    /* send the whisper */
    serv_chat_whisper(to, chat->id, from_username, message);

    return PURPLE_CMD_RET_OK;
  }
}

static void yggdrasilprpl_chat_whisper(PurpleConnection *gc, int id, const char *who,
                                  const char *message) {
  const char *username = gc->account->username;
  PurpleConversation *conv = purple_find_chat(gc, id);
  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "%s receives whisper from %s in chat room %s: %s\n",
                    username, who, conv->name, message);

  /* receive whisper on recipient's account */
  serv_got_chat_in(gc, id, who, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_WHISPER,
                   message, time(NULL));
}

static void receive_chat_message(PurpleConvChat *from, PurpleConvChat *to,
                                 int id, const char *room, gpointer userdata) {
  const char *message = (const char *)userdata;
  PurpleConnection *to_gc = get_yggdrasilprpl_gc(to->nick);

  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "%s receives message from %s in chat room %s: %s\n",
                    to->nick, from->nick, room, message);
  serv_got_chat_in(to_gc, id, from->nick, PURPLE_MESSAGE_RECV, message,
                   time(NULL));
}

static int yggdrasilprpl_chat_send(PurpleConnection *gc, int id, const char *message,
                              PurpleMessageFlags flags) {
  char *send_chat_cmd;
  char *escaped_message;
  const char *username = gc->account->username;
  PurpleConversation *conv = purple_find_chat(gc, id);
  PurpleConvChat *chat;
  int ret_val;

  if (conv) {
    purple_debug_info(PLUGIN_DEBUG_NAME,
                      "%s is sending message to chat room %s: %s\n", username,
                      conv->name, message);
    escaped_message = url_encode(message);
    send_chat_cmd = g_strdup_printf(
      "curl --silent \"http://yggdrasilradio.net/chatwrite.php?auth=%s&msg=%s\" | grep --silent 'OK'"
      , AUTH_CHAT
      , escaped_message
    );
    ret_val = system(send_chat_cmd);
    if( ret_val ){
      purple_notify_info(gc, _("Alert"), _("Alert"), _("chatwrite failed."));
    }
    g_free(send_chat_cmd);

    /* send message to everyone in the chat room */
    foreach_gc_in_chat(receive_chat_message, gc, id, (gpointer)message);

    chatread();
    chat = purple_conversation_get_chat_data(conv);
    yggdrasilprpl_chat_update_convo(chat);
    free(escaped_message);
    return 0;
  } else {
    purple_debug_info(PLUGIN_DEBUG_NAME,
                      "tried to send message from %s to chat room #%d: %s\n"
                      "but couldn't find chat room",
                      username, id, message);
    return -1;
  }
}

static void yggdrasilprpl_register_user(PurpleAccount *acct) {
 purple_debug_info(PLUGIN_DEBUG_NAME, "registering account for %s\n",
                   acct->username);
}

static void yggdrasilprpl_get_cb_info(PurpleConnection *gc, int id, const char *who) {
  PurpleConversation *conv = purple_find_chat(gc, id);
  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "retrieving %s's info for %s in chat room %s\n", who,
                    gc->account->username, conv->name);

  yggdrasilprpl_get_info(gc, who);
}

static void yggdrasilprpl_alias_buddy(PurpleConnection *gc, const char *who,
                                 const char *alias) {
 purple_debug_info(PLUGIN_DEBUG_NAME, "%s sets %s's alias to %s\n",
                   gc->account->username, who, alias);
}

static void yggdrasilprpl_group_buddy(PurpleConnection *gc, const char *who,
                                 const char *old_group,
                                 const char *new_group) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s has moved %s from group %s to group %s\n",
                    gc->account->username, who, old_group, new_group);
}

static void yggdrasilprpl_rename_group(PurpleConnection *gc, const char *old_name,
                                  PurpleGroup *group, GList *moved_buddies) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s has renamed group %s to %s\n",
                    gc->account->username, old_name, group->name);
}

static void yggdrasilprpl_convo_closed(PurpleConnection *gc, const char *who) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s's conversation with %s was closed\n",
                    gc->account->username, who);
}

/* normalize a username (e.g. remove whitespace, add default domain, etc.)
 * for yggdrasilprpl, this is a noop.
 */
static const char *yggdrasilprpl_normalize(const PurpleAccount *acct,
                                      const char *input) {
  return NULL;
}

static void yggdrasilprpl_set_buddy_icon(PurpleConnection *gc,
                                    PurpleStoredImage *img) {
 purple_debug_info(PLUGIN_DEBUG_NAME, "setting %s's buddy icon to %s\n",
                   gc->account->username,
                   img ? purple_imgstore_get_filename(img) : "(yggdrasil)");
}

static void yggdrasilprpl_remove_group(PurpleConnection *gc, PurpleGroup *group) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "%s has removed group %s\n",
                    gc->account->username, group->name);
}


static void set_chat_topic_fn(PurpleConvChat *from, PurpleConvChat *to,
                              int id, const char *room, gpointer userdata) {
  const char *topic = (const char *)userdata;
  const char *username = from->conv->account->username;
  char *msg;

  purple_conv_chat_set_topic(to, username, topic);

  if (topic && strlen(topic) > 0)
    msg = g_strdup_printf(_("%s sets topic to: %s"), username, topic);
  else
    msg = g_strdup_printf(_("%s clears topic"), username);

  purple_conv_chat_write(to, username, msg,
                         PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG,
                         time(NULL));
  g_free(msg);
}

static void yggdrasilprpl_set_chat_topic(PurpleConnection *gc, int id,
                                    const char *topic) {
  PurpleConversation *conv = purple_find_chat(gc, id);
  PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
  const char *last_topic;

  if (!chat)
    return;

  purple_debug_info(PLUGIN_DEBUG_NAME, "%s sets topic of chat room '%s' to '%s'\n",
                    gc->account->username, conv->name, topic);

  last_topic = purple_conv_chat_get_topic(chat);
  if ((!topic && !last_topic) ||
      (topic && last_topic && !strcmp(topic, last_topic)))
    return;  /* topic is unchanged, this is a noop */

  foreach_gc_in_chat(set_chat_topic_fn, gc, id, (gpointer)topic);
}

static gboolean yggdrasilprpl_finish_get_roomlist(gpointer roomlist) {
  purple_roomlist_set_in_progress((PurpleRoomlist *)roomlist, FALSE);
  return FALSE;
}

static PurpleRoomlist *yggdrasilprpl_roomlist_get_list(PurpleConnection *gc) {
  const char *username = gc->account->username;
  PurpleRoomlist *roomlist = purple_roomlist_new(gc->account);
  GList *fields = NULL;
  PurpleRoomlistField *field;
  GList *chats;
  GList *seen_ids = NULL;

  purple_debug_info(PLUGIN_DEBUG_NAME, "%s asks for room list; returning:\n", username);

  /* set up the room list */
  field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "room",
                                    "room", TRUE /* hidden */);
  fields = g_list_append(fields, field);

  field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, "Id", "Id", FALSE);
  fields = g_list_append(fields, field);

  purple_roomlist_set_fields(roomlist, fields);

  /* add each chat room. the chat ids are cached in seen_ids so that each room
   * is only returned once, even if multiple users are in it. */
  for (chats  = purple_get_chats(); chats; chats = g_list_next(chats)) {
    PurpleConversation *conv = (PurpleConversation *)chats->data;
    PurpleRoomlistRoom *room;
    const char *name = conv->name;
    int id = purple_conversation_get_chat_data(conv)->id;

    /* have we already added this room? */
    if (g_list_find_custom(seen_ids, name, (GCompareFunc)strcmp))
      continue;                                /* yes! try the next one. */

    /* This cast is OK because this list is only staying around for the life
     * of this function and none of the conversations are being deleted
     * in that timespan. */
    seen_ids = g_list_prepend(seen_ids, (char *)name); /* no, it's new. */
    purple_debug_info(PLUGIN_DEBUG_NAME, "%s (%d), ", name, id);

    room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, name, NULL);
    purple_roomlist_room_add_field(roomlist, room, name);
    purple_roomlist_room_add_field(roomlist, room, &id);
    purple_roomlist_room_add(roomlist, room);
  }

  g_list_free(seen_ids);
  purple_timeout_add(1 /* ms */, yggdrasilprpl_finish_get_roomlist, roomlist);
  return roomlist;
}

static void yggdrasilprpl_roomlist_cancel(PurpleRoomlist *list) {
 purple_debug_info(PLUGIN_DEBUG_NAME, "%s asked to cancel room list request\n",
                   list->account->username);
}

static void yggdrasilprpl_roomlist_expand_category(PurpleRoomlist *list,
                                              PurpleRoomlistRoom *category) {
 purple_debug_info(PLUGIN_DEBUG_NAME, "%s asked to expand room list category %s\n",
                   list->account->username, category->name);
}

/* yggdrasilprpl doesn't support file transfer...yet... */
static gboolean yggdrasilprpl_can_receive_file(PurpleConnection *gc,
                                          const char *who) {
  return FALSE;
}

static gboolean yggdrasilprpl_offline_message(const PurpleBuddy *buddy) {
  purple_debug_info(PLUGIN_DEBUG_NAME,
                    "reporting that offline messages are supported for %s\n",
                    buddy->name);

  return TRUE;
}

/*
 * prpl stuff. see prpl.h for more information.
 */

static PurplePluginProtocolInfo prpl_info =
{
  OPT_PROTO_CHAT_TOPIC,  /* options */
  NULL,               /* user_splits, initialized in yggdrasilprpl_init() */
  NULL,               /* protocol_options, initialized in yggdrasilprpl_init() */
  {   /* icon_spec, a PurpleBuddyIconSpec */
      "png,jpg,gif",                   /* format */
      0,                               /* min_width */
      0,                               /* min_height */
      128,                             /* max_width */
      128,                             /* max_height */
      10000,                           /* max_filesize */
      PURPLE_ICON_SCALE_DISPLAY,       /* scale_rules */
  },
  yggdrasilprpl_list_icon,                  /* list_icon */
  NULL,                                /* list_emblem */
  yggdrasilprpl_status_text,                /* status_text */
  yggdrasilprpl_tooltip_text,               /* tooltip_text */
  yggdrasilprpl_status_types,               /* status_types */
  yggdrasilprpl_blist_node_menu,            /* blist_node_menu */
  yggdrasilprpl_chat_info,                  /* chat_info */
  yggdrasilprpl_chat_info_defaults,         /* chat_info_defaults */
  yggdrasilprpl_login,                      /* login */
  yggdrasilprpl_close,                      /* close */
  yggdrasilprpl_send_im,                    /* send_im */
  yggdrasilprpl_set_info,                   /* set_info */
  yggdrasilprpl_send_typing,                /* send_typing */
  yggdrasilprpl_get_info,                   /* get_info */
  yggdrasilprpl_set_status,                 /* set_status */
  yggdrasilprpl_set_idle,                   /* set_idle */
  yggdrasilprpl_change_passwd,              /* change_passwd */
  yggdrasilprpl_add_buddy,                  /* add_buddy */
  yggdrasilprpl_add_buddies,                /* add_buddies */
  yggdrasilprpl_remove_buddy,               /* remove_buddy */
  yggdrasilprpl_remove_buddies,             /* remove_buddies */
  yggdrasilprpl_add_permit,                 /* add_permit */
  yggdrasilprpl_add_deny,                   /* add_deny */
  yggdrasilprpl_rem_permit,                 /* rem_permit */
  yggdrasilprpl_rem_deny,                   /* rem_deny */
  yggdrasilprpl_set_permit_deny,            /* set_permit_deny */
  yggdrasilprpl_join_chat,                  /* join_chat */
  yggdrasilprpl_reject_chat,                /* reject_chat */
  yggdrasilprpl_get_chat_name,              /* get_chat_name */
  yggdrasilprpl_chat_invite,                /* chat_invite */
  yggdrasilprpl_chat_leave,                 /* chat_leave */
  yggdrasilprpl_chat_whisper,               /* chat_whisper */
  yggdrasilprpl_chat_send,                  /* chat_send */
  NULL,                                /* keepalive */
  yggdrasilprpl_register_user,              /* register_user */
  yggdrasilprpl_get_cb_info,                /* get_cb_info */
  NULL,                                /* get_cb_away */
  yggdrasilprpl_alias_buddy,                /* alias_buddy */
  yggdrasilprpl_group_buddy,                /* group_buddy */
  yggdrasilprpl_rename_group,               /* rename_group */
  NULL,                                /* buddy_free */
  yggdrasilprpl_convo_closed,               /* convo_closed */
  yggdrasilprpl_normalize,                  /* normalize */
  yggdrasilprpl_set_buddy_icon,             /* set_buddy_icon */
  yggdrasilprpl_remove_group,               /* remove_group */
  NULL,                                /* get_cb_real_name */
  yggdrasilprpl_set_chat_topic,             /* set_chat_topic */
  NULL,                                /* find_blist_chat */
  yggdrasilprpl_roomlist_get_list,          /* roomlist_get_list */
  yggdrasilprpl_roomlist_cancel,            /* roomlist_cancel */
  yggdrasilprpl_roomlist_expand_category,   /* roomlist_expand_category */
  yggdrasilprpl_can_receive_file,           /* can_receive_file */
  NULL,                                /* send_file */
  NULL,                                /* new_xfer */
  yggdrasilprpl_offline_message,            /* offline_message */
  NULL,                                /* whiteboard_prpl_ops */
  NULL,                                /* send_raw */
  NULL,                                /* roomlist_room_serialize */
  NULL,                                /* unregister_user */
  NULL,                                /* send_attention */
  NULL,                                /* get_attention_types */
  sizeof(PurplePluginProtocolInfo),    /* struct_size */
  NULL,                                /* get_account_text_table */
  NULL,                                /* initiate_media */
  NULL,                                /* get_media_caps */
  NULL,                                /* get_moods */
  NULL,                                /* set_public_alias */
  NULL,                                /* get_public_alias */
  NULL,                                /* add_buddy_with_invite */
  NULL                                 /* add_buddies_with_invite */
};

static void yggdrasilprpl_init(PurplePlugin *plugin)
{
  int ret;
  /* see accountopt.h for information about user splits and protocol options */
  PurpleAccountOption *option = purple_account_option_string_new(
    _("Example option"),      /* text shown to user */
    "example",                /* pref name */
    "default");               /* default value */

  purple_debug_info(PLUGIN_DEBUG_NAME, "starting up\n");

  prpl_info.protocol_options = g_list_append(NULL, option);

  /* register whisper chat command, /msg */
  purple_cmd_register("msg",
                    "ws",                  /* args: recipient and message */
                    PURPLE_CMD_P_DEFAULT,  /* priority */
                    PURPLE_CMD_FLAG_CHAT,
                    "prpl-yggdrasil",
                    send_whisper,
                    "msg &lt;username&gt; &lt;message&gt;: send a private message, aka a whisper",
                    NULL);                 /* userdata */

  /* get ready to store offline messages */
  goffline_messages = g_hash_table_new_full(g_str_hash,  /* hash fn */
                                            g_str_equal, /* key comparison fn */
                                            g_free,      /* key free fn */
                                            NULL);       /* value free fn */

  ret = system("echo '' > /tmp/yggdrasil.chat.log.txt");
  if(ret) printf("problem?");
  _yggdrasil_protocol = plugin;
}

static void yggdrasilprpl_destroy(PurplePlugin *plugin) {
  purple_debug_info(PLUGIN_DEBUG_NAME, "shutting down\n");
}


static PurplePluginInfo info =
{
  PURPLE_PLUGIN_MAGIC,                                     /* magic */
  PURPLE_MAJOR_VERSION,                                    /* major_version */
  PURPLE_MINOR_VERSION,                                    /* minor_version */
  PURPLE_PLUGIN_PROTOCOL,                                  /* type */
  NULL,                                                    /* ui_requirement */
  0,                                                       /* flags */
  NULL,                                                    /* dependencies */
  PURPLE_PRIORITY_DEFAULT,                                 /* priority */
  YGGDRASILPRPL_ID,                                        /* id */
  "Yggdrasil Intercom",                                    /* name */
  DISPLAY_VERSION,                                         /* version */
  N_("Yggdrasil Intercom"),                                /* summary */
  N_("Yggdrasil Intercom"),                                /* description */
  "Victor Yap <victor.yap@alumni.concordia.ca>",           /* author */
  "https://github.com/starlocke/yggdrasilprpl",            /* homepage */
  NULL,                                                    /* load */
  NULL,                                                    /* unload */
  yggdrasilprpl_destroy,                                    /* destroy */
  NULL,                                                    /* ui_info */
  &prpl_info,                                              /* extra_info */
  NULL,                                                    /* prefs_info */
  yggdrasilprpl_actions,                                    /* actions */
  NULL,                                                    /* padding... */
  NULL,
  NULL,
  NULL,
};

PURPLE_INIT_PLUGIN(yggdrasil, yggdrasilprpl_init, info);
