#pragma once
#include <Arduino.h>
#include <vector>
#include <libssh/libssh.h>

// Public keys allowed to log in, stored in flash as /authorized_keys (OpenSSH format,
// one "type base64 comment" line per key).
#define MAX_AUTHORIZED_KEYS 8

struct AuthorizedKey {
  String type;
  String base64;
  String comment;
};

std::vector<AuthorizedKey> authKeysLoad();
// Parses an OpenSSH public key line; on success adds it and returns true.
bool authKeysAdd(const String& line, String& err, String& fingerprint);
bool authKeysDelete(size_t index);  // index counts from 1, as shown by `key list`
// Returns true and the matching key's comment if `key` is authorized.
bool authKeysFind(ssh_key key, String& comment);

String sshKeyFingerprint(ssh_key key);  // "SHA256:..."
String authKeyFingerprint(const AuthorizedKey& k);
