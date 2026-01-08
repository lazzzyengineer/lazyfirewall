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

## How LazyFirewall Works

LazyFirewall is a native NGINX Web Application Firewall implemented as a C module,
with request decision logic handled by an external Go engine.

NGINX remains the enforcement point at all times.

---

### On NGINX Startup

- The LazyFirewall module is loaded into NGINX
- A handler is registered in the HTTP ACCESS phase
- A shared memory zone is initialized to track backend health
- No changes to existing location blocks are required

---

### On Each Request

- The request reaches the ACCESS phase
- LazyFirewall extracts request metadata:
  - client IP
  - host header
  - HTTP method
  - full unparsed URI
- This data is serialized as JSON
- The JSON payload is sent to the Go engine over a UNIX socket
- The engine replies with a decision:
  - `block`
  - any other value means allow
- If the response is `block`, NGINX immediately returns HTTP 403
- Otherwise, the request continues normally

---

### Backend Health Handling

- Backend failures are tracked in shared memory
- If the engine becomes unavailable:
  - a cooldown window is activated
  - new requests skip engine calls during this period
- Behavior during engine failure is configurable:
  - fail open: requests are allowed
  - fail closed: requests are blocked

---

### Concurrency Model

- Each NGINX worker process maintains its own persistent socket connection
- Workers are single-threaded, so no per-request locking is required
- Shared memory is used only for health signaling between workers
- Mutex protection ensures safe cross-worker updates

---

### Why This Design

- Zero per-request NGINX subrequests
- No dependency on `auth_request`
- No configuration changes required per location
- Low latency and predictable performance
- Clear separation of enforcement and decision logic

`
Client
  ↓
NGINX (C module, ACCESS phase)
  ↓ (metadata JSON over Unix socket)
Go decision engine
  ↓
NGINX allow / block `

---

### Role of the Go Engine

The Go engine acts as the policy and intelligence layer.

It can:
- implement rate limiting
- perform IP reputation checks
- apply behavioral rules
- integrate with external systems
- evolve independently of NGINX

NGINX enforces the decision, the engine decides.


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
- add POST body inspection

### By the end of next phase:

- NGINX never hangs
- Firewall has timeouts
- Fail-open / fail-close is configurable
- Engine is reusable & fast
- Behavior is predictable under load

---

## Philosophy

Security should be automatic.
Security should be hard to bypass.
Security should never cause downtime.

---

Built by LazyEngineer
