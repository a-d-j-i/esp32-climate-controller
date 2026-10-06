#include "auth_keys.h"
#include "config.h"
#include "state.h"
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <vector>

static const char* KEYS_PATH = "/authorized_keys";

static ssh_key importKey(const AuthorizedKey& k) {
  ssh_key key = nullptr;
  enum ssh_keytypes_e type = ssh_key_type_from_name(k.type.c_str());
  if (type == SSH_KEYTYPE_UNKNOWN) return nullptr;
  if (ssh_pki_import_pubkey_base64(k.base64.c_str(), type, &key) != SSH_OK) return nullptr;
  return key;
}

// Bits in an RSA public key's modulus, read from the OpenSSH key data
// (string "ssh-rsa", integer e, integer n, each with a 4-byte length). 0 if unreadable.
static int rsaBits(const String& base64) {
  std::vector<uint8_t> blob(base64.length());
  size_t len = 0;
  if (mbedtls_base64_decode(blob.data(), blob.size(), &len, (const uint8_t*)base64.c_str(), base64.length()) != 0)
    return 0;
  size_t pos = 0;
  uint32_t fieldLen = 0;
  for (int field = 0; field < 3; field++) {
    if (pos + 4 > len) return 0;
    fieldLen = (uint32_t)blob[pos] << 24 | blob[pos + 1] << 16 | blob[pos + 2] << 8 | blob[pos + 3];
    pos += 4;
    if (fieldLen > len - pos) return 0;
    if (field < 2) pos += fieldLen;
  }
  while (fieldLen && blob[pos] == 0) {  // leading zero byte keeps the integer positive
    pos++;
    fieldLen--;
  }
  if (!fieldLen) return 0;
  int bits = fieldLen * 8;
  for (uint8_t top = blob[pos]; !(top & 0x80); top <<= 1) bits--;
  return bits;
}

static bool parseLine(String line, AuthorizedKey& k) {
  line.trim();
  if (!line.length() || line[0] == '#') return false;
  int sp1 = line.indexOf(' ');
  if (sp1 < 0) return false;
  int sp2 = line.indexOf(' ', sp1 + 1);
  k.type = line.substring(0, sp1);
  k.base64 = sp2 < 0 ? line.substring(sp1 + 1) : line.substring(sp1 + 1, sp2);
  k.comment = sp2 < 0 ? "" : line.substring(sp2 + 1);
  k.base64.trim();
  k.comment.trim();
  return k.base64.length() > 0;
}

std::vector<AuthorizedKey> authKeysLoad() {
  std::vector<AuthorizedKey> keys;
  if (!LittleFS.exists(KEYS_PATH)) return keys;  // open() would log an error
  File f = LittleFS.open(KEYS_PATH, "r");
  if (!f) return keys;
  while (f.available()) {
    AuthorizedKey k;
    if (parseLine(f.readStringUntil('\n'), k)) keys.push_back(k);
  }
  f.close();
  return keys;
}

static bool save(const std::vector<AuthorizedKey>& keys) {
  File f = LittleFS.open(KEYS_PATH, "w");
  if (!f) return false;
  for (const auto& k : keys) f.print(k.type + " " + k.base64 + (k.comment.length() ? " " + k.comment : "") + "\n");
  f.close();
  return true;
}

String sshKeyFingerprint(ssh_key key) {
  String out = "?";
  unsigned char* hash = nullptr;
  size_t len = 0;
  if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len) == SSH_OK) {
    char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, len);
    if (fp) {
      out = fp;
      ssh_string_free_char(fp);
    }
    ssh_clean_pubkey_hash(&hash);
  }
  return out;
}

String authKeyFingerprint(const AuthorizedKey& k) {
  ssh_key key = importKey(k);
  if (!key) return "(invalid key)";
  String fp = sshKeyFingerprint(key);
  ssh_key_free(key);
  return fp;
}

bool authKeysAdd(const String& line, String& err, String& fingerprint) {
  AuthorizedKey k;
  if (!parseLine(line, k)) {
    err = "expected an OpenSSH public key line, e.g. the contents of ~/.ssh/id_ed25519.pub";
    return false;
  }
  ssh_key key = importKey(k);
  if (!key) {
    err = "could not read a '" + k.type + "' key (supported: ssh-ed25519, ecdsa-sha2-nistp256/384/521, ssh-rsa of 2048 bits or more)";
    return false;
  }
  int bits = ssh_key_type(key) == SSH_KEYTYPE_RSA ? rsaBits(k.base64) : RSA_MIN_BITS;
  if (bits < RSA_MIN_BITS) {
    err = "RSA key has " + String(bits) + " bits; at least " + String(RSA_MIN_BITS) + " are required";
    ssh_key_free(key);
    return false;
  }
  String comment;
  bool duplicate = authKeysFind(key, comment);
  fingerprint = sshKeyFingerprint(key);
  ssh_key_free(key);
  if (duplicate) {
    err = "this key is already authorized";
    return false;
  }
  Lock lk(g_lock);
  auto keys = authKeysLoad();
  if (keys.size() >= MAX_AUTHORIZED_KEYS) {
    err = "at most " + String(MAX_AUTHORIZED_KEYS) + " keys; delete one first";
    return false;
  }
  keys.push_back(k);
  if (!save(keys)) {
    err = "could not write to flash";
    return false;
  }
  return true;
}

bool authKeysDelete(size_t index) {
  Lock lk(g_lock);
  auto keys = authKeysLoad();
  if (index < 1 || index > keys.size()) return false;
  keys.erase(keys.begin() + (index - 1));
  return save(keys);
}

bool authKeysFind(ssh_key key, String& comment) {
  for (const auto& k : authKeysLoad()) {
    ssh_key candidate = importKey(k);
    if (!candidate) continue;
    bool match = ssh_key_cmp(candidate, key, SSH_KEY_CMP_PUBLIC) == 0;
    ssh_key_free(candidate);
    if (match) {
      comment = k.comment;
      return true;
    }
  }
  return false;
}
