# HTTP Server

A webpage served with the standard library's `HttpServer`. The server owns
the accept loop and all HTTP framing (request parsing, status line,
`Content-Length`); the handler lambda only inspects `HttpRequest` and fills
in `HttpResponse`.

Build:

```bash
./build.sh
```

Run and open http://127.0.0.1:8080 in a browser:

```bash
./server
```

Or verify from another terminal:

```bash
curl -i http://127.0.0.1:8080/         # 200 with the page
curl -i http://127.0.0.1:8080/missing  # 404
```
