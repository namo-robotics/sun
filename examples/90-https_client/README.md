---
docs-page: examples
---

# HTTPS Client

An HTTPS request with `HttpsClient` from the `tls` bundle. The client opens a
TLS connection, sends an HTTP/1.1 request and parses the response, decoding
either a `Content-Length` or a chunked body.

```sun
using sun;
using tls;

var client = HttpsClient(alloc);
var host = String(alloc, "example.com");
var response = client.get(host, "/");
```

Certificates are verified by default: the chain must validate against the
system CA store and the certificate must match the hostname, otherwise
`connect` throws. Point `SSL_CERT_FILE` at a PEM file to trust a private CA
instead.

`tls.moon` carries its own static OpenSSL, so nothing here needs `-lssl`,
`-lcrypto`, or OpenSSL installed on the machine — and the compiled binary is
self-contained:

```bash
./build.sh
./client
ldd client   # not a dynamic executable
```

Building it yourself requires the bundle, which the normal build produces
once `./scripts/fetch-openssl.sh` has supplied the archives.
