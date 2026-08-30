# relay-bridge — plain-HTTP → HTTPS proxy for the SIM800L

## Why this exists

The scanner-uno's SIM800L cannot speak TLS — see `scanner-uno/include/config.h` and
`scanner-uno/src/modem.h`. Real TLS needs BearSSL/mbedTLS-class RAM that a 2 KB AVR does not have,
the same reason the tracker's SIM800L terminates TLS on the ESP32 instead of trusting the modem's
own `AT+CIPSSL`. There is no equivalent headroom on the Uno to do it on-MCU either.

So the Uno can only ever speak plain HTTP, and Supabase only ever accepts HTTPS. This is what sits
between them: a small, dumb reverse proxy. It holds **no secrets** — Supabase Edge Functions already
validate `DEVICE_TOKEN` themselves and hold their own service-role access internally, so this only
needs to move bytes from a plain socket to a TLS one, unchanged. It forwards exactly one path prefix,
`/functions/v1/`, and 404s everything else — it is not a general-purpose open proxy, which is a real
distinction: an nginx `proxy_pass` on `/` would happily forward arbitrary internet traffic to
arbitrary hosts if someone found it and asked it to.

## The one thing that decides where this lives

**The scanner reaches this box over the SIM800L's cellular data connection, not your LAN.** That
means this box needs a public IP or a public DNS name — "run it on a machine in the server room"
only works if that machine is also reachable from the public internet. Two ways to get there:

- **A cheap VPS** (DigitalOcean, Vultr, Linode, etc. — roughly $4-6/mo for the smallest tier). No
  networking to fight; get a static IP, point `SUPABASE_HOST` at your project, done. This is the
  path of least resistance and what most of this project's "small footprint" hosting decisions
  have favored.
- **On-prem with port forwarding.** If the gate site has a static public IP (or a dynamic-DNS
  service on top of a dynamic one) and you can forward port 80 through its router to this
  machine, that works too. More moving parts to keep working over time — the port forward
  survives a router reboot but not always a router *replacement* — but zero recurring cost.

A machine on the school's Wi-Fi with no port forwarding and no public IP — the "just run it
locally" instinct — is the one option that does **not** work here, because nothing on the cellular
side can reach it. (This is unrelated to `server/`'s local FastAPI dev server, which the ESP32
scanner reaches over the school's own Wi-Fi and doesn't have this constraint at all.)

## Deploying it

```bash
cd relay-bridge
SUPABASE_HOST=YOUR-REF.supabase.co docker compose up -d
```

Or put `SUPABASE_HOST=YOUR-REF.supabase.co` in a `.env` file next to `docker-compose.yml` instead,
so a `git pull` never clobbers a real deployment's config by resetting it to the placeholder.

Verify it's actually proxying, not just running — the same check that caught a real bug during
development (see below):

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://YOUR-BOX/functions/v1/roster   # expect 401/403, not a timeout — that means it reached Supabase
curl -s -o /dev/null -w "%{http_code}\n" http://YOUR-BOX/anything-else         # expect 404 from OUR nginx
```

## Then point the scanner at it

In `scanner-uno/include/config.h`:

```c
#define API_HOST      "YOUR-BOX-IP-OR-DOMAIN"
static constexpr uint16_t API_PORT = 80;
```

## What was actually verified, and what wasn't

This was tested end to end during development — not just eyeballed. Pointing `SUPABASE_HOST` at
`httpbin.org` and comparing response bodies proved the proxied path really does leave the
container and reach a real HTTPS host (httpbin's own 404 page came back, not nginx's), while an
unproxied path stayed local (nginx's bare 404). That test also caught a real bug: nginx's
`proxy_pass` resolves a bare hostname **once, at startup**, and refuses to even start if it can't
resolve it — which would have meant this container crash-looping on a bad hostname, and worse,
silently going stale forever if Supabase's IP ever rotates after a successful start. Fixed with
the `resolver` + variable-based `proxy_pass` in `templates/default.conf.template` — see the
comment there.

What was **not** verified: an actual round trip against a real Supabase project (none exists yet
— `supabase/README.md`'s setup hasn't been run), and reachability from an actual SIM800L on a real
PH carrier's cellular data connection. Confirm both before trusting this at a real gate.
