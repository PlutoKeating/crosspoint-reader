#!/usr/bin/env bash
# Builds wolfSSL natively with the device's TLS configuration (the WOLFSSL/HAVE
# flags from platformio.ini [base] plus the patched Arduino user_settings.h)
# and performs a real TLS 1.3 handshake + GET with the pinned roots from
# src/project_stick/StudioTrust.h. Catches certificate-chain, hash, group and
# cipher incompatibilities with the live server without hardware.
#
#   scripts/tls_bench.sh [host] [path]
# Needs a prior `pio run -e gh_release` (for .pio/libdeps/.../Arduino-wolfSSL).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${1:-stockstick.plutokeating.beer}"
URLPATH="${2:-/api/v1/public/firmware/version?channel=stable}"
WSRC="$(ls -d "$ROOT"/.pio/libdeps/*/Arduino-wolfSSL/src | head -1)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
python3 - "$ROOT" "$OUT" <<'PY'
import re, sys
root, out = sys.argv[1], sys.argv[2]
ini = open(f"{root}/platformio.ini").read()
base = ini.split("[base]", 1)[1].split("\n[", 1)[0]
flags = re.findall(r"^\s*(-D(?:WOLFSSL|HAVE|WC)_[A-Z0-9_]+(?:=\S+)?)", base, re.M)
open(f"{out}/flags.txt", "w").write(" ".join(flags))
pem = re.search(r'R"PEM\((.*?)\)PEM"', open(f"{root}/src/project_stick/StudioTrust.h").read(), re.S).group(1)
open(f"{out}/roots.pem", "w").write(pem)
PY
cat > "$OUT/client.c" <<'C'
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
int main(int argc, char** argv) {
  const char *host = argv[1], *path = argv[2], *roots = argv[3];
  wolfSSL_Init();
  struct addrinfo hints = {0}, *res;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, "443", &hints, &res)) { puts("DNS failed"); return 1; }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fd, res->ai_addr, res->ai_addrlen)) { puts("TCP connect failed"); return 1; }
  WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
  wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);
  if (wolfSSL_CTX_load_verify_buffer(ctx, (const unsigned char*)roots, strlen(roots), WOLFSSL_FILETYPE_PEM) != 1) {
    puts("loading the pinned roots failed"); return 1;
  }
  WOLFSSL* ssl = wolfSSL_new(ctx);
  wolfSSL_check_domain_name(ssl, host);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));
  wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519);  /* as SecureClient does */
  wolfSSL_set_fd(ssl, fd);
  int ret = wolfSSL_connect(ssl);
  if (ret != WOLFSSL_SUCCESS) {
    char msg[80]; int err = wolfSSL_get_error(ssl, ret);
    printf("HANDSHAKE FAILED: %d %s\n", err, wolfSSL_ERR_error_string(err, msg)); return 2;
  }
  printf("handshake ok: %s %s\n", wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl));
  char req[512]; snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
  wolfSSL_write(ssl, req, strlen(req));
  static char resp[8192]; int n, total = 0;
  while ((n = wolfSSL_read(ssl, resp + total, sizeof(resp) - 1 - total)) > 0) total += n;
  resp[total] = 0;
  char* body = strstr(resp, "\r\n\r\n");
  printf("%.*s\n%s\n", (int)(strchr(resp, '\r') - resp), resp, body ? body + 4 : "");
  return 0;
}
C
mkdir -p "$OUT/inc/wolfssl" && : > "$OUT/inc/wolfssl/options.h"
SRCS=$(ls "$WSRC"/src/*.c "$WSRC"/wolfcrypt/src/*.c | grep -v -E "sp_arm|sp_cortexm|sp_armthumb|sp_dsp32|sp_x86|/bio.c|/conf.c|/pk.c|/x509.c|/x509_str.c|/ssl_|/misc.c|/evp.c|/fips|/selftest|/async|/port/" | tr "\n" " ")
# shellcheck disable=SC2046,SC2086
gcc -O1 -w -include sys/time.h -DWOLFSSL_USER_SETTINGS -DWOLFSSL_OPTIONS_H -DWOLFSSL_CLIENT_EXAMPLE \
  $(cat "$OUT/flags.txt") -I"$OUT/inc" -I"$WSRC" $SRCS "$OUT/client.c" -o "$OUT/client" -lm
"$OUT/client" "$HOST" "$URLPATH" "$(cat "$OUT/roots.pem")" 2>&1 | grep -v -E "^(wolfSSL|\s)"
