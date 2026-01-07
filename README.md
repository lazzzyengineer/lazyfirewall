# LazyFirewall

LazyFirewall is an automatic Layer-7 firewall for NGINX.

It uses:
- a small C NGINX module for wiring
- a Go engine for decision making

The firewall is enabled by default and requires no per-location config.

---

## Why LazyFirewall

NGINX security is usually manual and repetitive.
LazyFirewall secures all locations automatically and safely.

Key ideas:
- C only wires things together
- Go decides allow or block
- NGINX enforces the result
- failure never breaks traffic

---

## How it works

On NGINX startup:
- the module walks all HTTP locations
- it injects auth_request everywhere
- existing auth_request is preserved and chained

For each request:
- NGINX asks the Go engine
- the engine replies allow or block
- NGINX enforces the decision

If the engine is down:
- requests are allowed
- no downtime

---

## auth_request compatibility

NGINX supports only one auth_request per location.

LazyFirewall handles this safely:
- existing auth_request is detected
- the Go engine calls the original auth first
- firewall logic runs after

No authentication is broken.

---

## Build

Build the module against the same NGINX source version.

./configure --with-compat --add-dynamic-module=../ngx_lazyfirewall  
make

The module file is created in the objs directory.

---

## Install (macOS example)

Copy the module to the NGINX modules directory.

Add this line at the top of nginx.conf.

load_module modules/ngx_http_lazyfirewall_module.so;

Reload NGINX.

---

## Go engine

Start the Go engine.

It listens on a Unix socket in /tmp.

NGINX talks to it for every request.

---

## Security model

Default behavior is allow.
Only explicit block decisions deny traffic.
Engine failure never blocks requests.

---

## Roadmap

- enable or disable directive
- IP based rules
- rate limiting
- metrics
- Linux packages
- installer automation

---

## Philosophy

Security should be automatic.
Security should be hard to bypass.
Security should never cause downtime.

---

Built by LazyEngineer
