description: Patchbays that decide what may leave: a WAF for NATS with regex payload scanning, a p99 outlier gate and flood detection, and de-identification of patient records.

# Filtering and privacy

{lede} Most examples reshape a stream. These decide what is allowed through: a WAF for NATS that scans payloads with a regular expression, holds back readings above the rolling p99 and flags a flooding subject, and a patchbay that strips and masks identifiers in patient records before they are forwarded.

## A WAF for NATS

{tag:Intermediate} A web application firewall sits in front of a web server and decides which requests get through. This patchbay does the same job for a NATS cluster. Publishers connect to monoblok instead of the cluster and publish under `in.>`; three rules decide what continues as `clean.>` and what is reported on `waf.>`.

{{include examples/waf.edn}}

```sh
examples/waf.sh
```

Rules see a message's subject and payload. They do not see which connection published it, so every decision here is per subject, not per client.

### Payload scan

> **Experimental.** The regex forms used here, `matches?`, `re-capture` and `re-replace`, are new; the forms and pattern syntax may change between releases.

The first rule is stateless. [`matches?`](forms.html#regular-expressions) tests the payload against one case-insensitive pattern, and `if` sends the message one of two ways. The `(?i)` prefix ignores case, `\s+` accepts any run of whitespace between `drop` and `table`, and the three alternatives are a script tag, a `${jndi:` lookup and a table drop. A blocked event is not forwarded and its payload is not repeated: `waf.blocked` carries only the subject it arrived on, so one subscriber sees every block without receiving the content that caused it.

```soundcheck examples/waf.edn
in.event.orders|{"note":"leave at the door"}
in.event.orders|{"note":"<script>alert(1)</script>"}
in.event.signup|{"name":"Ada"}
in.event.signup|{"name":"x; DROP TABLE users"}
in.event.orders|{"note":"<SCRIPT src=x>"}
in.event.signup|{"name":"x; drop   table users"}
in.event.orders|{"note":"${jndi:ldap://host/a}"}
```

The pattern is a string literal, written `#"\s+"` in EDN and `'\s+'` in a `.yml` file. There is no decoding, so a payload that hides `<script` behind URL or HTML escapes passes. Treat the rule as a cheap first filter, not a full inspection layer.

### Outlier gate

The second rule forwards a reading only while it is at or below the [`percentile`](forms.html#rate-percentiles-distribution-shape) of the last 100 readings from that device. There is no fixed limit to maintain: each device learns its own normal, because state is per subject.

```soundcheck examples/waf.edn
in.reading.pump7|412
in.reading.pump7|398
in.reading.pump7|405
in.reading.pump7|391
in.reading.pump7|388
in.reading.pump7|9000
in.reading.pump7|402
in.reading.pump7|395
in.reading.pump7|64000
in.reading.pump7|399
```

Ten readings in, eight out. Two things to know before relying on it:

- **The window includes the current sample.** A reading that sets a new maximum for the window is above the p99 by definition, so it is held back. On a steady device that is about one message in a hundred. On a device whose readings only climb, it is every message.
- **Held-back samples still count.** The 9000 above stays in the window for the next 100 samples and raises the p99 a little. A sustained run of large values therefore becomes the new normal. Add a fixed ceiling with `(and (< payload-float 8192) ...)` when there is a hard limit as well.

### Flood detection

The third rule watches how fast each subject publishes. [`rate`](forms.html#rate-percentiles-distribution-shape) counts messages per second over a five-second window, and [`transition`](forms.html#special-forms) turns the comparison into two messages: one `block` when the rate rises above 2 per second (more than ten messages in five seconds) and one `clear` when it falls back.

```soundcheck examples/waf.edn
in.event.chatty|tick 1
in.event.chatty|tick 2
in.event.chatty|tick 3
in.event.chatty|tick 4
in.event.chatty|tick 5
in.event.chatty|tick 6
in.event.chatty|tick 7
in.event.chatty|tick 8
in.event.chatty|tick 9
in.event.chatty|tick 10
in.event.chatty|tick 11
in.event.quiet|hello
```

The eleventh message from `in.event.chatty` crosses the threshold; `in.event.quiet` has its own window and stays quiet.

- **One window per subject, with no bookkeeping.** State is per call site and subject. Each subject that has ever published holds one slot, so keep the subject space bounded.
- **The verdict does not stop the traffic.** The ticks above are still forwarded as `clean.event.chatty`. `waf.flood.<subject>` is advice for something that can act on it, such as revoking the publisher's credentials. To cap the flow in the rule itself, put [`throttle`](forms.html#time-based-gates) in the forwarding thread.
- **`clear` needs a message.** A rule runs when a message arrives, so `clear` is published on the first message after the subject has slowed down, not at the moment the window empties.

## De-identifying records

{tag:Intermediate} A bedside gateway publishes one JSON frame per reading on `ward.<ward>.<patient>`, with the patient's name and record number alongside the measurements. The reporting system needs the measurements and must not receive the identifiers.

{{include examples/deidentify.edn}}

```soundcheck examples/deidentify.edn
ward.icu.p4471|{"name":"Ada Example","mrn":"000-11-2222","age":47,"hr":72,"spo2":97}
ward.icu.p9020|{"name":"Bo Sample","mrn":"000-33-4444","age":81,"hr":90,"spo2":93}
notes.icu|Comfortable overnight, obs stable
notes.icu|Call family re MRN 000-11-2222
notes.icu|Discharge review booked, born 3/4/1980
notes.icu|MRN 4471, check chart
```

What to notice:

- **An allow-list, not a block-list.** [`json-get`](forms.html#json-payloads) reads the fields the rule names. `name` and `mrn` are not removed; they are never read. A field the gateway starts sending next month is left behind too, where a pattern that hunts for identifiers would have to learn about it first.
- **The subject is data too.** `ward.icu.p4471` identifies a patient as surely as the name does. The rules publish under `clean.<ward>.…`, so every patient on a ward shares one output subject.
- **Coarsen what identifies.** [`quantize 10`](forms.html#idempotent-filters) snaps age to the nearest ten. `round` does the same job for coordinates.
- **Aggregate where a total is enough.** The ward average is a funnel: the first rule is `:reentrant true`, so its `clean.icu.hr` output feeds the `moving-avg` rule.
- **Free text gets masked, then checked.** A note cannot be allow-listed field by field. The first notes rule uses [`re-replace`](forms.html#regular-expressions) (an experimental form) to swap a record number shaped like `000-11-2222` for `[MRN]` and a date like `3/4/1980` for `[DATE]`, then hands the masked text to a second rule through `masked.notes.<ward>`. That second rule holds any note that still names `MRN`, `DOB` or `NHS` followed by something the masks did not cover, such as `MRN 4471`, and reports only the subject.
- **Masking only catches the shapes it is given.** A name, an address, or a record number in another format passes straight through. The allow-list above is still the stronger tool wherever the data has fields.

Structured frames are de-identified by omission, not obfuscation. Patchbay has no hashing or tokenising form, so it cannot turn a record number into a stable pseudonym. If downstream needs to follow one patient across messages, issue the pseudonym at the gateway and put it in the subject. Rules like these reduce what leaves the ward; they are not a compliance control by themselves.

```sh
examples/deidentify.sh
```

## Making it a gate

The rules above only decide. What makes the patchbay a gate is that the raw subjects have no other way out. Two arrangements give you that:

- **Front door.** Publishers connect to monoblok, and a top-level `export` names the only subjects forwarded to the cluster:

```edn
(export
  :servers ["nats://nats.internal:4222"]
  :export  ["clean.>" "waf.>" "phi.>"])
```

- **Tap.** With [`import`](bridge.html), monoblok subscribes to the raw subjects on the remote server. Imported messages are private patchbay input: local subscribers see only what the rules publish.

Both are described in [Export, import and JetStream](bridge.html), with runnable versions in [NATS integration](examples-nats.html).
