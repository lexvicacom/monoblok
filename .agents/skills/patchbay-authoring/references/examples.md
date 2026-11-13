# Worked Patchbay examples

## Worked examples

```edn
; Fan high readings onto a dedicated sub-subject.
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish! (subject-append "high") payload)))

; Mirror anything that looks like an alert onto events.alerts.
(on ">"
  (when (contains? payload "alert")
    (publish! "events.alerts" (str-concat subject ": " payload))))

; Experimental regex forms: pull the number out of "t=42 ok" and forward it.
; The pattern is a string literal; #"..." is EDN's raw string.
(on "dev.*.status"
  (-> payload
      (re-capture #"t=(\d+)")
      (publish! (subject-append "temp"))))

; Clean up an RPM stream: 50rpm buckets, drop duplicates.
(on "car.*.rpm"
  (-> payload-float
      (quantize 50)
      (squelch)
      (publish! (subject-append "stable"))))

; Smooth + deadband in one pipeline.
(on "sensors.*"
  (-> payload-float
      (moving-avg 10)
      (deadband 1.0)
      (publish! (subject-append "smoothed"))))

; Time-windowed smoothing: 5-second sliding average. Old samples drop
; off on the next push or on the slot's deadline timer.
(on "sensors.*"
  (-> payload-float
      (moving-avg :ms 5000)
      (publish! (subject-append "avg5s"))))

; Clocked liveness + trailing/cadence values.
(on "devices.*.heartbeat"
  (dropout :ms 30000
    :lost  (publish! (subject-append "stale") "true")
    :found (publish! (subject-append "stale") "false")))

(on "dials.*"
  (debounce! :ms 250 (subject-append "settled") payload))

(on "sensors.*"
  (sample! :ms 1000 (subject-append "sampled") payload))

(on "sensors.*"
  (aggregate! :ms 5000 (subject-append "avg5s") :avg payload-float))

; Sustained-heat alert and all-clear, one rule, one ring.
(on "temp.*.*"
  (transition (> (moving-avg 60 payload-float) 28.0)
    (publish! (subject-append "alert") "hot")
    (publish! (subject-append "ok")    "cool")))

; Volatility: two windows, non-threaded form.
(on "sensors.*"
  (when (> (- (moving-max 20 payload-float)
              (moving-min 20 payload-float)) 5.0)
    (publish! (subject-append "volatile") payload)))

; 1-minute aligned OHLC bars on a market feed. Bars close on the next
; tick that crosses a wall-clock minute boundary, or from the slot's
; deadline timer if the feed goes quiet.
(on "MARKET.*"
  (bar! :ms 60000 payload-float))

; Live events/sec per subject, useful for monitoring dashboards.
(on "events.>"
  (-> payload (rate :ms 1000) (publish! (subject-append "hz"))))

; Tail-latency monitor: only flag rules where p99 over the last 200
; samples exceeds a threshold.
(on "rpc.latency.*"
  (when (> (percentile 200 0.99 payload-float) 250.0)
    (publish! (subject-append "slow") payload)))

; At most 5 alerts per minute, regardless of how they arrive.
(on "alerts.>"
  (-> payload
      (throttle :ms 60000 5)
      (publish! (subject-append "rate-limited"))))
```
