#include "ssh_shell.h"
#include "auth_keys.h"
#include "commands.h"
#include "config.h"
#include "console_session.h"
#include "line_editor.h"
#include "settings.h"
#include "state.h"
#include <lwip/sockets.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>
#include <libssh/server.h>

static String s_fingerprint = "(not generated yet)";
static uint32_t s_consecutiveFailures = 0;

// Only modern algorithms; anything else is refused during the key exchange.
static const char* KEX = "curve25519-sha256,curve25519-sha256@libssh.org";
static const char* CIPHERS =
    "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes128-ctr";
static const char* MACS = "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512";
static const char* USER_KEY_TYPES =
    "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";

// Bounds the next blocking libssh call by what is left of the login deadline.
static bool armTimeout(ssh_session session, uint32_t deadline) {
  int32_t left = (int32_t)(deadline - millis());
  if (left <= 0) return false;
  long seconds = left / 1000 + 1;
  ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &seconds);
  return true;
}

static bool credentialsConfigured() { return settingsHasPassword() || !authKeysLoad().empty(); }

String sshHostKeyFingerprint() { return "ED25519 " + s_fingerprint; }

// The host key is generated on first boot and kept in flash, so clients see the same
// key after reboots and firmware updates.
static ssh_key loadOrCreateHostKey() {
  ssh_key key = nullptr;
  String stored = settingsLoadHostKey();
  if (stored.length() &&
      ssh_pki_import_privkey_base64(stored.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK)
    return key;
  logEvent("generating Secure Shell host key");
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) return nullptr;
  char* b64 = nullptr;
  if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &b64) == SSH_OK) {
    settingsSaveHostKey(b64);
    ssh_string_free_char(b64);
  }
  return key;
}

static void computeFingerprint(ssh_key key) {
  ssh_key pub = nullptr;
  if (ssh_pki_export_privkey_to_pubkey(key, &pub) != SSH_OK) return;
  s_fingerprint = sshKeyFingerprint(pub);
  ssh_key_free(pub);
}

static String peerAddress(ssh_session session) {
  struct sockaddr_in addr;
  socklen_t len = sizeof addr;
  if (getpeername(ssh_get_fd(session), (struct sockaddr*)&addr, &len) != 0) return "?";
  return IPAddress(addr.sin_addr.s_addr).toString();
}

static bool passwordLoginAllowed() {
  bool enabled;
  {
    Lock lk(g_lock);
    enabled = g_settings.passwordLogin;
  }
  return enabled && settingsHasPassword();
}

static bool authenticate(ssh_session session, const String& peer, uint32_t deadline) {
  int failures = 0;
  bool wrongUserLogged = false;
  for (int messages = 0; failures < SSH_MAX_AUTH_TRIES && messages < 20; messages++) {
    if (!armTimeout(session, deadline)) return false;
    ssh_message msg = ssh_message_get(session);
    if (!msg) return false;
    if (ssh_message_type(msg) == SSH_REQUEST_AUTH) {
      const char* user = ssh_message_auth_user(msg);
      bool userOk = user && strcmp(user, SSH_USER) == 0;
      if (!userOk && !wrongUserLogged && ssh_message_subtype(msg) != SSH_AUTH_METHOD_NONE) {
        wrongUserLogged = true;
        logEvent("Secure Shell: login as unknown user '%s' from %s (the user is '%s')", user ? user : "",
                 peer.c_str(), SSH_USER);
      }
      bool passwordAllowed = passwordLoginAllowed();
      int sub = ssh_message_subtype(msg);

      if (sub == SSH_AUTH_METHOD_PUBLICKEY) {
        // The client first asks whether a key would be accepted (state NONE), then
        // proves it holds the private key; libssh checks the signature (state VALID).
        ssh_key key = ssh_message_auth_pubkey(msg);
        auto state = ssh_message_auth_publickey_state(msg);
        String comment;
        if (userOk && key && authKeysFind(key, comment)) {
          if (state == SSH_PUBLICKEY_STATE_NONE) {
            ssh_message_auth_reply_pk_ok_simple(msg);
            ssh_message_free(msg);
            continue;
          }
          if (state == SSH_PUBLICKEY_STATE_VALID) {
            ssh_message_auth_reply_success(msg, 0);
            ssh_message_free(msg);
            logEvent("Secure Shell: key '%s' accepted from %s", comment.c_str(), peer.c_str());
            s_consecutiveFailures = 0;
            return true;
          }
        }
      } else if (sub == SSH_AUTH_METHOD_PASSWORD && passwordAllowed) {
        const char* pass = ssh_message_auth_password(msg);
        if (userOk && pass && settingsCheckPassword(pass)) {
          ssh_message_auth_reply_success(msg, 0);
          ssh_message_free(msg);
          logEvent("Secure Shell: password accepted from %s", peer.c_str());
          s_consecutiveFailures = 0;
          return true;
        }
        failures++;
        s_consecutiveFailures++;
        logEvent("Secure Shell: failed password login as '%s' from %s", user ? user : "", peer.c_str());
        // Sessions are served one at a time, so this delay limits guessing across all
        // connections: 1 s after the first failure, growing to SSH_FAIL_DELAY_MAX_S.
        delay(min<uint32_t>(s_consecutiveFailures, SSH_FAIL_DELAY_MAX_S) * 1000);
      }
      ssh_message_auth_set_methods(
          msg, SSH_AUTH_METHOD_PUBLICKEY | (passwordAllowed ? SSH_AUTH_METHOD_PASSWORD : 0));
    }
    ssh_message_reply_default(msg);
    ssh_message_free(msg);
  }
  return false;
}

struct ChannelSetup {
  ssh_channel chan = nullptr;
  bool pty = false;
  bool exec = false;
  String command;
};

static bool openChannel(ssh_session session, ChannelSetup& cs, uint32_t deadline) {
  for (int messages = 0; messages < 30; messages++) {
    if (!armTimeout(session, deadline)) return false;
    ssh_message msg = ssh_message_get(session);
    if (!msg) return false;
    int type = ssh_message_type(msg);
    int sub = ssh_message_subtype(msg);
    if (type == SSH_REQUEST_CHANNEL_OPEN && sub == SSH_CHANNEL_SESSION && !cs.chan) {
      cs.chan = ssh_message_channel_request_open_reply_accept(msg);
      ssh_message_free(msg);
      continue;
    }
    if (type == SSH_REQUEST_CHANNEL && cs.chan) {
      bool start = sub == SSH_CHANNEL_REQUEST_SHELL || sub == SSH_CHANNEL_REQUEST_EXEC;
      if (sub == SSH_CHANNEL_REQUEST_PTY) cs.pty = true;
      if (sub == SSH_CHANNEL_REQUEST_EXEC) {
        const char* c = ssh_message_channel_request_command(msg);
        cs.command = c ? c : "";
        cs.exec = true;
      }
      if (start || sub == SSH_CHANNEL_REQUEST_PTY || sub == SSH_CHANNEL_REQUEST_ENV ||
          sub == SSH_CHANNEL_REQUEST_WINDOW_CHANGE) {
        ssh_message_channel_request_reply_success(msg);
        ssh_message_free(msg);
        if (start) return true;
        continue;
      }
    }
    ssh_message_reply_default(msg);
    ssh_message_free(msg);
  }
  return false;
}

static void write(ssh_channel chan, bool pty, const String& text) {
  String t = pty ? toCrlf(text) : text;
  ssh_channel_write(chan, t.c_str(), t.length());
}

static uint32_t sshIdleMinutes() {
  Lock lk(g_lock);
  return g_settings.sshIdleMin;
}

// Lets the network stack notice a client that disappeared without closing the
// connection; reads then fail and the session ends (see SSH_KEEPALIVE_* in config.h).
static void enableKeepalive(ssh_session session) {
  int fd = ssh_get_fd(session);
  int on = 1, idle = SSH_KEEPALIVE_IDLE_S, interval = SSH_KEEPALIVE_INTERVAL_S, count = SSH_KEEPALIVE_COUNT;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
}

static void runInteractive(ssh_channel chan, bool pty) {
  // Without a terminal (ssh -T) the client echoes and sends whole lines, so there is
  // no line editing and live lines are simply printed.
  ConsoleSession session(Console::Ssh, pty, [&](const String& s) { write(chan, pty, s); });
  session.start();
  uint32_t lastActivity = millis();  // typing only: output such as watch lines does not count
  char buf[128];
  for (;;) {
    int n = ssh_channel_read_timeout(chan, buf, sizeof buf, 0, 100);
    if (n == SSH_ERROR || ssh_channel_is_eof(chan) || ssh_channel_is_closed(chan)) return;
    uint32_t idleMin = sshIdleMinutes();
    if (n > 0) {
      lastActivity = millis();
      if (!session.feed(buf, n)) return;
    } else if (idleMin && millis() - lastActivity > idleMin * 60000UL) {
      session.poll();
      write(chan, pty, "\nidle timeout\n");
      return;
    }
    session.poll();
  }
}

// `ssh host 'cmd1; cmd2'` runs the commands; exit status 1 if any failed.
static void runExec(ssh_channel chan, bool pty, const String& command) {
  Out out = [&](const String& s) { write(chan, pty, s); };
  if (command.length() > MAX_COMMAND_LINE) {
    out("error: command too long\n");
    ssh_channel_request_send_exit_status(chan, 1);
    return;
  }
  int status = 0;
  int start = 0;
  while (start <= (int)command.length()) {
    int end = command.indexOf(';', start);
    if (end < 0) end = command.length();
    CmdResult r = runCommand(command.substring(start, end), Console::Ssh, out);
    if (r == CmdResult::Error) status = 1;
    if (r == CmdResult::Exit) break;
    start = end + 1;
  }
  ssh_channel_request_send_exit_status(chan, status);
}

static void handleSession(ssh_session session) {
  String peer = peerAddress(session);
  enableKeepalive(session);
  uint32_t deadline = millis() + SSH_LOGIN_TIMEOUT_S * 1000UL;
  armTimeout(session, deadline);
  if (ssh_handle_key_exchange(session) != SSH_OK) {
    logEvent("Secure Shell: key exchange with %s failed: %s", peer.c_str(), ssh_get_error(session));
    return;
  }
  if (!authenticate(session, peer, deadline)) return;
  ChannelSetup cs;
  if (openChannel(session, cs, deadline)) {
    if (cs.exec) runExec(cs.chan, cs.pty, cs.command);
    else runInteractive(cs.chan, cs.pty);
    ssh_channel_send_eof(cs.chan);
    ssh_channel_close(cs.chan);
  }
  if (cs.chan) ssh_channel_free(cs.chan);
}

static void sshTask(void*) {
  ssh_key key = loadOrCreateHostKey();
  if (!key) {
    logEvent("Secure Shell: no host key, server not started");
    vTaskDelete(nullptr);
    return;
  }
  computeFingerprint(key);
  logEvent("Secure Shell host key %s", sshHostKeyFingerprint().c_str());

  // No default credentials: the port stays closed until the owner sets a password or
  // adds a key on the USB console.
  if (!credentialsConfigured()) {
    logEvent("Secure Shell off: set a password (passwd) or add a key (key add) on the USB console");
    while (!credentialsConfigured()) vTaskDelay(pdMS_TO_TICKS(2000));
  }

  ssh_bind bind = ssh_bind_new();
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT_STR, SSH_PORT);
  ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key);  // bind takes ownership
  const std::pair<ssh_bind_options_e, const char*> algorithms[] = {
      {SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS, "ssh-ed25519"},
      {SSH_BIND_OPTIONS_KEY_EXCHANGE, KEX},
      {SSH_BIND_OPTIONS_CIPHERS_C_S, CIPHERS},
      {SSH_BIND_OPTIONS_CIPHERS_S_C, CIPHERS},
      {SSH_BIND_OPTIONS_HMAC_C_S, MACS},
      {SSH_BIND_OPTIONS_HMAC_S_C, MACS},
      {SSH_BIND_OPTIONS_PUBKEY_ACCEPTED_KEY_TYPES, USER_KEY_TYPES},
  };
  int rsaMinBits = RSA_MIN_BITS;
  if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_RSA_MIN_SIZE, &rsaMinBits) < 0)
    logEvent("Secure Shell: could not set the RSA minimum size: %s", ssh_get_error(bind));
  for (const auto& a : algorithms) {
    if (ssh_bind_options_set(bind, a.first, a.second) < 0) {
      logEvent("Secure Shell: algorithm setup failed (%s), server not started", ssh_get_error(bind));
      vTaskDelete(nullptr);
      return;
    }
  }
  while (ssh_bind_listen(bind) < 0) {
    logEvent("Secure Shell: listen failed: %s", ssh_get_error(bind));
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
  logEvent("Secure Shell listening on port %s", SSH_PORT);
  for (;;) {
    ssh_session session = ssh_new();
    if (ssh_bind_accept(bind, session) == SSH_OK) {
      handleSession(session);
      ssh_disconnect(session);
    }
    ssh_free(session);
  }
}

void sshBegin() {
  libssh_begin();
  xTaskCreatePinnedToCore(sshTask, "ssh", SSH_TASK_STACK, nullptr, 1, nullptr, CONSOLE_CORE);
}
