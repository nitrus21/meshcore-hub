// Telegram bot texts. To add a language: add a column to every entry and a code in LANGS.
#pragma once
#include <string.h>

namespace tgs {

enum Id {
  WELCOME, LOCKED, HELP, NOT_READY, UNKNOWN_CONTACT, UNKNOWN_CHANNEL, AMBIGUOUS, SENT_TO, TOO_LONG,
  USAGE_MSG, USAGE_CH, USAGE_LOGIN, USAGE_MUTE, REPLY_HINT, PICK_CONTACT, NOW_WRITE, ADVERT_SENT,
  LOGIN_SENT, MUTED, UNMUTED, NOT_DELIVERED, RESEND, NO_CONTACTS, NO_CHANNELS, CONTACTS_TITLE,
  CHANNELS_TITLE, STATUS_TITLE, MODULE, BATTERY, WIFI, UPTIME, UNREAD, YOU, DIRECT, HOPS, MODULE_DOWN,
  MODULE_UP, TOPIC_GENERAL_HINT, CMD_HELP, CMD_MSG, CMD_CH, CMD_PUBLIC, CMD_CONTACTS, CMD_CHANNELS,
  CMD_STATUS, CMD_ADVERT, CMD_LOGIN, CMD_MUTE, CMD_ADDCH, CMD_DELCH, USAGE_ADDCH, CH_ADDED, CH_ADDED_HASHTAG,
  CH_ADDED_PRIVATE, CH_DEL_CONFIRM, CH_DEL_BUTTON, PICK_DELCH, CH_DELETED, CH_REFUSED, CH_ERR_EXISTS, CH_ERR_FULL,
  CH_ERR_PUBLIC, CH_ERR_NAME, CH_ERR_KEY, CH_ERR_OFFLINE, CH_ERR_RETRY, COUNT
};

static const char* const LANGS[] = {"fr", "en"};

// clang-format off
static const char* const T[COUNT][2] = {
  /*WELCOME*/ {"✅ <b>MeshCore Hub</b> est relié à cette conversation.\nLes messages MeshCore arriveront ici. Réponds à un message pour répondre sur le réseau.\n/aide pour la liste des commandes.",
               "✅ <b>MeshCore Hub</b> is now linked to this chat.\nMeshCore messages will arrive here. Reply to a message to answer on the mesh.\n/help for the command list."},
  /*LOCKED*/  {"Ce bot est déjà relié à une autre conversation.", "This bot is already linked to another chat."},
  /*HELP*/    {"<b>Commandes</b>\n/msg &lt;nom&gt; &lt;texte&gt; – message privé\n/ch &lt;canal&gt; &lt;texte&gt; – message sur un canal\n/public &lt;texte&gt; – canal public\n/contacts – liste des contacts\n/canaux – liste des canaux\n/nouveaucanal #nom – rejoindre un canal public par son nom\n/nouveaucanal &lt;nom&gt; – créer un canal privé\n/supprcanal &lt;canal&gt; – supprimer un canal\n/status – état du Hub et du module\n/advert – s'annoncer sur le réseau\n/login &lt;room&gt; &lt;mot de passe&gt; – se connecter à une room\n/sourdine &lt;conversation&gt; – ne plus relayer (ou réactiver)\n\nAstuce : <b>réponds</b> à un message relayé pour répondre au bon contact.",
               "<b>Commands</b>\n/msg &lt;name&gt; &lt;text&gt; – private message\n/ch &lt;channel&gt; &lt;text&gt; – channel message\n/public &lt;text&gt; – public channel\n/contacts – contact list\n/channels – channel list\n/addchannel #name – join a public channel by name\n/addchannel &lt;name&gt; – create a private channel\n/delchannel &lt;channel&gt; – delete a channel\n/status – Hub and module status\n/advert – announce yourself on the mesh\n/login &lt;room&gt; &lt;password&gt; – log into a room\n/mute &lt;conversation&gt; – stop relaying (or resume)\n\nTip: <b>reply</b> to a relayed message to answer the right contact."},
  /*NOT_READY*/ {"⚠️ Le module MeshCore n'est pas connecté : message gardé, il partira à la reconnexion.", "⚠️ The MeshCore module is not connected: message queued, it will go out on reconnection."},
  /*UNKNOWN_CONTACT*/ {"Contact introuvable : %s", "Unknown contact: %s"},
  /*UNKNOWN_CHANNEL*/ {"Canal introuvable : %s", "Unknown channel: %s"},
  /*AMBIGUOUS*/ {"Plusieurs correspondances, précise le nom.", "Several matches, please be more specific."},
  /*SENT_TO*/ {"→ %s", "→ %s"},
  /*TOO_LONG*/ {"⚠️ Message trop long (%u octets, max %u).", "⚠️ Message too long (%u bytes, max %u)."},
  /*USAGE_MSG*/ {"Utilisation : /msg &lt;nom&gt; &lt;texte&gt;", "Usage: /msg &lt;name&gt; &lt;text&gt;"},
  /*USAGE_CH*/ {"Utilisation : /ch &lt;canal&gt; &lt;texte&gt;", "Usage: /ch &lt;channel&gt; &lt;text&gt;"},
  /*USAGE_LOGIN*/ {"Utilisation : /login &lt;room&gt; &lt;mot de passe&gt;", "Usage: /login &lt;room&gt; &lt;password&gt;"},
  /*USAGE_MUTE*/ {"Utilisation : /sourdine &lt;conversation&gt;", "Usage: /mute &lt;conversation&gt;"},
  /*REPLY_HINT*/ {"Pour envoyer, <b>réponds</b> à un message relayé, ou utilise /msg, /ch ou /public.", "To send, <b>reply</b> to a relayed message, or use /msg, /ch or /public."},
  /*PICK_CONTACT*/ {"À qui veux-tu écrire ?", "Who do you want to write to?"},
  /*NOW_WRITE*/ {"✍️ Écris ton message pour %s :", "✍️ Type your message for %s:"},
  /*ADVERT_SENT*/ {"📣 Annonce envoyée sur le réseau.", "📣 Advert sent on the mesh."},
  /*LOGIN_SENT*/ {"🔑 Connexion à %s en cours…", "🔑 Logging into %s…"},
  /*MUTED*/ {"🔕 %s ne sera plus relayé.", "🔕 %s will no longer be relayed."},
  /*UNMUTED*/ {"🔔 %s est de nouveau relayé.", "🔔 %s is relayed again."},
  /*NOT_DELIVERED*/ {"⚠️ Non délivré à %s", "⚠️ Not delivered to %s"},
  /*RESEND*/ {"🔁 Renvoyer", "🔁 Resend"},
  /*NO_CONTACTS*/ {"Aucun contact.", "No contacts."},
  /*NO_CHANNELS*/ {"Aucun canal.", "No channels."},
  /*CONTACTS_TITLE*/ {"<b>Contacts</b>", "<b>Contacts</b>"},
  /*CHANNELS_TITLE*/ {"<b>Canaux</b>", "<b>Channels</b>"},
  /*STATUS_TITLE*/ {"<b>MeshCore Hub</b>", "<b>MeshCore Hub</b>"},
  /*MODULE*/ {"Module", "Module"},
  /*BATTERY*/ {"Batterie", "Battery"},
  /*WIFI*/ {"Wi-Fi", "Wi-Fi"},
  /*UPTIME*/ {"Allumé depuis", "Uptime"},
  /*UNREAD*/ {"Non lus", "Unread"},
  /*YOU*/ {"Vous", "You"},
  /*DIRECT*/ {"direct", "direct"},
  /*HOPS*/ {"sauts", "hops"},
  /*MODULE_DOWN*/ {"🔌 Module MeshCore déconnecté.", "🔌 MeshCore module disconnected."},
  /*MODULE_UP*/ {"📡 Module MeshCore connecté : %s", "📡 MeshCore module connected: %s"},
  /*TOPIC_GENERAL_HINT*/ {"Écris dans le sujet d'une conversation, ou utilise /msg.", "Write in a conversation topic, or use /msg."},
  /*CMD_HELP*/ {"Aide et liste des commandes", "Help and command list"},
  /*CMD_MSG*/ {"Message privé à un contact", "Private message to a contact"},
  /*CMD_CH*/ {"Message sur un canal", "Message on a channel"},
  /*CMD_PUBLIC*/ {"Message sur le canal public", "Message on the public channel"},
  /*CMD_CONTACTS*/ {"Liste des contacts", "Contact list"},
  /*CMD_CHANNELS*/ {"Liste des canaux", "Channel list"},
  /*CMD_STATUS*/ {"État du Hub et du module", "Hub and module status"},
  /*CMD_ADVERT*/ {"S'annoncer sur le réseau", "Announce yourself"},
  /*CMD_LOGIN*/ {"Se connecter à une room", "Log into a room"},
  /*CMD_MUTE*/ {"Couper / réactiver une conversation", "Mute / unmute a conversation"},
  /*CMD_ADDCH*/ {"Ajouter un canal (#nom ou nom privé)", "Add a channel (#name or private name)"},
  /*CMD_DELCH*/ {"Supprimer un canal", "Delete a channel"},
  /*USAGE_ADDCH*/ {"<b>Ajouter un canal</b>\n/nouveaucanal #nom – canal public par nom (même clé pour tous ceux qui tapent ce nom)\n/nouveaucanal &lt;nom&gt; – canal privé avec une clé au hasard\n/nouveaucanal &lt;nom&gt; &lt;clé de 32 caractères&gt; – rejoindre un canal privé\nOu colle un lien <code>meshcore://channel/add?…</code>",
                   "<b>Add a channel</b>\n/addchannel #name – public channel by name (same key for everyone typing that name)\n/addchannel &lt;name&gt; – private channel with a random key\n/addchannel &lt;name&gt; &lt;32-character key&gt; – join a private channel\nOr paste a <code>meshcore://channel/add?…</code> link"},
  /*CH_ADDED*/ {"✅ Canal <b>%s</b> ajouté.", "✅ Channel <b>%s</b> added."},
  /*CH_ADDED_HASHTAG*/ {"Tous ceux qui connaissent ce nom peuvent lire ce canal.", "Anyone who knows this name can read this channel."},
  /*CH_ADDED_PRIVATE*/ {"Lien à partager pour inviter quelqu'un :", "Link to share to invite someone:"},
  /*CH_DEL_CONFIRM*/ {"Supprimer le canal <b>%s</b> du module ?\nSes messages enregistrés sur le Hub seront effacés.", "Delete channel <b>%s</b> from the module?\nIts messages stored on the Hub will be erased."},
  /*CH_DEL_BUTTON*/ {"🗑 Supprimer", "🗑 Delete"},
  /*PICK_DELCH*/ {"Quel canal supprimer ?", "Which channel should be deleted?"},
  /*CH_DELETED*/ {"🗑 Canal <b>%s</b> supprimé.", "🗑 Channel <b>%s</b> deleted."},
  /*CH_REFUSED*/ {"❌ Le module a refusé (%s).", "❌ The module refused (%s)."},
  /*CH_ERR_EXISTS*/ {"Ce canal existe déjà.", "This channel already exists."},
  /*CH_ERR_FULL*/ {"Plus d'emplacement libre sur le module : supprime d'abord un canal.", "No free slot on the module: delete a channel first."},
  /*CH_ERR_PUBLIC*/ {"Le canal public ne peut pas être supprimé.", "The public channel cannot be deleted."},
  /*CH_ERR_NAME*/ {"Nom invalide (31 caractères maximum).", "Invalid name (31 characters max)."},
  /*CH_ERR_KEY*/ {"Clé invalide : il faut 32 caractères hexadécimaux (0-9, a-f).", "Invalid key: 32 hexadecimal characters needed (0-9, a-f)."},
  /*CH_ERR_OFFLINE*/ {"⚠️ Le module MeshCore n'est pas connecté.", "⚠️ The MeshCore module is not connected."},
  /*CH_ERR_RETRY*/ {"La liste des canaux est en cours de relecture sur le module, réessaie dans quelques secondes.", "The channel list is being re-read from the module, try again in a few seconds."},
};
// clang-format on

inline int langIndex(const char* lang) {
  for (int i = 0; i < (int)(sizeof(LANGS) / sizeof(LANGS[0])); i++)
    if (strcmp(lang, LANGS[i]) == 0) return i;
  return 0;
}

}  // namespace tgs
