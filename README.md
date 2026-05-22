# monoblok

**NATS-native signal conditioning.**

Clean, filter, and transform noisy data **once** at the edge — instead of scattering the same logic across dozens of services.

### Problem

Raw high-frequency streams (sensors, market data, IoT, vehicles, etc.) are messy.  
Most teams end up writing custom cleaning code in every subscriber. That’s slow, error-prone, and wasteful.

**monoblok** lets you declare all your signal conditioning rules in one place using a simple configuration file. It then sits neatly in your NATS estate and does the rest — extremely fast.

### Key Features

- **Blazing fast** — Up to **15+ million messages/sec** on modest hardware (written in C and libuv)
- **Truly NATS-native** — Full import/export subject support, JetStream, LVC, snapshots
- **Tiny** — Minimal memory footprint, perfect for edge and sidecar deployments
- **Patchbay DSL** — Clean, declarative rules as YAML or EDN (easy to write by hand or with AI)
- **Flexible deployment** — Sidecar, front-door proxy, or standalone mode
- **Production ready** — TLS, Basic Auth, systemd support

### Also check out **tinyblok**

For microcontrollers: **[tinyblok](https://github.com/lexvicacom/tinyblok)** — Lightweight Patchbay that runs directly on ESP32 and publishes conditioned data to NATS.


### Quick Start (under 30 seconds)

```bash
# Install & run
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash

# Run with example config
./monoblok --port 14222
```

Publish raw data, subscribe to clean subjects. That’s it.

See the [`examples/`](./examples/) folder for ready-to-use configs.



### Common Use Cases

- **Industrial / IoT** — Clean noisy sensor data before it hits your backend
- **Financial** — Turn raw ticks into usable bars and alerts
- **Edge Computing** — Reduce bandwidth and cloud costs dramatically by only exporting _interesting_ data points
- **Microservices** — Eliminate duplicated transformation logic


### Deployment Modes

| Mode              | Use Case                              | When to use |
|-------------------|---------------------------------------|-------------|
| **Sidecar**       | Tap into existing subjects            | Most common |
| **Front door**    | Publishers send data through monoblok | Existing noisy publishers tamed |
| **Standalone**    | Lightweight broker + conditioning     | Small setups |

---

### Documentation
- [Overview](./docs/overview.md)
- [Designs/future](./design)
- [Patchbay DSL Guide](./docs/patchbay.md)** ← Start here
- [Examples](./examples/)
- [Moree examples](./advanced-examples/)
- [Benchmarks](./bench-results/)
- [Agent Instructions](./docs/AGENTS_PATCHBAY.md)

---

### Benchmarks

Consistently achieves **millions of messages per second** even with moderate conditioning rules.  
Full [benchmark scripts](./scripts) and results are included in the repo.