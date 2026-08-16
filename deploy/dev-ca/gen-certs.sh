#!/usr/bin/env sh
# Generate a throwaway dev root CA and a server certificate for the local
# OpenChat stack. The server cert is mounted into Caddy (TLS terminator); the
# root CA cert is what an OpenChat client (or curl) trusts/pins to reach the
# stack over HTTPS.
#
# DEV ONLY. Never use this CA or these certificates in production. Every key and
# certificate is written under ./out/, which is gitignored and never committed.
#
# Idempotent: existing material is reused. Pass --force to regenerate from
# scratch.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR="$SCRIPT_DIR/out"

DAYS_CA=3650
DAYS_SRV=825

FORCE=0
if [ "${1:-}" = "--force" ]; then
	FORCE=1
fi

mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"

CA_KEY="$OUT_DIR/rootCA.key"
CA_CRT="$OUT_DIR/rootCA.crt"
SRV_KEY="$OUT_DIR/server.key"
SRV_CRT="$OUT_DIR/server.crt"
SRV_CSR="$OUT_DIR/server.csr"
SRV_EXT="$OUT_DIR/server.ext"

if [ "$FORCE" -eq 1 ]; then
	rm -f "$CA_KEY" "$CA_CRT" "$SRV_KEY" "$SRV_CRT" "$SRV_CSR" "$SRV_EXT" \
		"$OUT_DIR/rootCA.srl"
fi

# 1) Root CA (self-signed).
if [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CRT" ]; then
	openssl req -x509 -newkey rsa:4096 -sha256 -days "$DAYS_CA" -nodes \
		-keyout "$CA_KEY" -out "$CA_CRT" \
		-subj "/O=OpenChat Dev/CN=OpenChat Dev Root CA"
	echo "Generated dev root CA."
else
	echo "Reusing existing dev root CA."
fi

# 2) Server certificate signed by the dev CA, with SANs for the local hostnames.
if [ ! -f "$SRV_KEY" ] || [ ! -f "$SRV_CRT" ]; then
	openssl req -newkey rsa:2048 -sha256 -nodes \
		-keyout "$SRV_KEY" -out "$SRV_CSR" \
		-subj "/O=OpenChat Dev/CN=localhost"

	cat > "$SRV_EXT" <<'EOF'
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = relay.localhost
DNS.3 = caddy
IP.1 = 127.0.0.1
IP.2 = ::1
EOF

	openssl x509 -req -in "$SRV_CSR" -CA "$CA_CRT" -CAkey "$CA_KEY" \
		-CAcreateserial -days "$DAYS_SRV" -sha256 \
		-extfile "$SRV_EXT" -out "$SRV_CRT"
	rm -f "$SRV_CSR" "$SRV_EXT"
	echo "Generated dev server certificate."
else
	echo "Reusing existing dev server certificate."
fi

chmod 600 "$OUT_DIR"/*.key

echo
echo "Dev CA and server certificate are ready in:"
echo "  $OUT_DIR"
echo
echo "Trust this root CA in the OpenChat client (or curl --cacert):"
echo "  $CA_CRT"
