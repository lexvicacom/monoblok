const patchbayExamples = {
  clean: {
    yaml: `on:
  - sub: "sensors.*"
    thread:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, [subject-append, "stable"]]`,
    code: '(on "sensors.*"\n  (-> payload-float\n      (round 1)\n      (squelch)\n      (publish! (subject-append "stable"))))',
    title: 'Round readings and suppress repeats',
    description: 'Round to one decimal place, then publish only when that rounded value changes.',
    inputLabel: 'sensors.temp', outputLabel: 'sensors.temp.stable',
    input: ['21.01', '21.02', '21.04', '21.12'], output: ['21.0', '21.1']
  },
  smooth: {
    yaml: `on:
  - sub: "sensors.*"
    thread:
      - $payload-float
      - [moving-avg, 10]
      - [deadband, 1.0]
      - [publish!, [subject-append, "smoothed"]]`,
    code: '(on "sensors.*"\n  (-> payload-float\n      (moving-avg 10)\n      (deadband 1.0)\n      (publish! (subject-append "smoothed"))))',
    title: 'Average readings before publishing',
    description: 'Average the last ten samples. Publish when that average moves by at least one degree from the last emitted value.',
    inputLabel: 'sensors.temp · first four samples', outputLabel: 'sensors.temp.smoothed',
    input: ['20.0', '20.4', '19.8', '24.6'], output: ['20.0', '21.2']
  },
  alert: {
    yaml: `on:
  - sub: "sensors.*"
    form: [transition, [">", $payload-float, 28.0], [publish!, [subject-append, "alert"], "hot"], [publish!, [subject-append, "ok"], "cool"]]`,
    code: '(on "sensors.*"\n  (transition (> payload-float 28.0)\n    (publish! (subject-append "alert") "hot")\n    (publish! (subject-append "ok") "cool")))',
    title: 'Publish on threshold crossings',
    description: 'Publish “hot” when the reading crosses above 28 and “cool” when it crosses back, with no messages between crossings.',
    inputLabel: 'sensors.temp', outputLabel: '.alert / .ok · after an initial reading of 27',
    input: ['29', '30', '29', '27'], output: ['hot', 'cool']
  },
  // live: false marks examples the public demo server does not run yet.
  steady: {
    yaml: `on:
  - sub: "sensors.*"
    form: [transition, [hysteresis, 27, 29, $payload-float], [publish!, [subject-append, "alert"], "hot"], [publish!, [subject-append, "ok"], "cool"]]`,
    code: '(on "sensors.*"\n  (transition (hysteresis 27 29 payload-float)\n    (publish! (subject-append "alert") "hot")\n    (publish! (subject-append "ok") "cool")))',
    title: 'Alert without the flapping',
    description: 'Turn on at 29 and off only at 27. Readings that hover around a single threshold no longer flip the alert on and off.',
    inputLabel: 'sensors.temp', outputLabel: '.alert / .ok · after an initial reading of 26',
    input: ['29.5', '27.6', '28.4', '26.5'], output: ['hot', 'cool'],
    live: false
  },
  funnel: {
    yaml: `on:
  - sub: "sensors.*"
    reentrant: true
    form: [publish!, "all.sensors", $payload]
  - sub: "all.sensors"
    form: [publish!, "sensors.average", dp, 1, [moving-avg, 4, $payload-float]]`,
    code: '(on "sensors.*" :reentrant true\n  (publish! "all.sensors" payload))\n\n(on "all.sensors"\n  (publish! "sensors.average" :dp 1\n    (moving-avg 4 payload-float)))',
    title: 'Funnel many subjects into one',
    description: 'Route readings from all sensors onto one subject to calculate a combined average.',
    inputLabel: 'sensors.kitchen, sensors.hall · alternating', outputLabel: 'sensors.average',
    input: ['20', '22', '21', '23'], output: ['20.0', '21.0', '21.0', '21.5'],
    live: false
  },
  join: {
    yaml: `lvc: ["temp.>"]

on:
  - sub: "temp.inside"
    thread:
      - [last, "temp.outside"]
      - ["-", $payload-float]
      - [publish!, "temp.difference", dp, 1]`,
    code: '(lvc ["temp.>"])\n\n(on "temp.inside"\n  (-> (last "temp.outside")\n      (- payload-float)\n      (publish! "temp.difference" :dp 1)))',
    title: 'Use the latest value from another subject',
    description: 'Read the last outside temperature while handling an inside reading, and publish the difference between the two.',
    inputLabel: 'temp.inside · with temp.outside at 18', outputLabel: 'temp.difference',
    input: ['21.5', '22.0', '20.5'], output: ['3.5', '4.0', '2.5'],
    live: false
  },
  ask: {
    yaml: `lvc: ["sensors.>"]

on:
  - sub: "ask.temp"
    form: [reply!, [last, "sensors.temp"]]`,
    code: '(lvc ["sensors.>"])\n\n(on "ask.temp"\n  (reply! (last "sensors.temp")))',
    title: 'Answer a request',
    description: 'Reply to a NATS request with the last reading, so a client can ask for the current value instead of waiting for the next one.',
    inputLabel: 'sensors.temp, then a request on ask.temp', outputLabel: 'reply to the requester',
    input: ['21.2', '21.4', 'request'], output: ['21.4'],
    live: false
  }
};

const patchbayOperators = /^(on|round|squelch|publish!|subject-append|moving-avg|deadband|transition|hysteresis|lvc|last|reply!)$/;

function renderPatchbayCode(container, code) {
  container.replaceChildren();
  for (const token of code.split(/("[^"]*"|[A-Za-z][A-Za-z0-9!?-]*)/g)) {
    const span = document.createElement('span');
    span.textContent = token;
    if (token.startsWith('"')) span.className = 'syntax-string';
    else if (patchbayOperators.test(token)) span.className = 'syntax-operator';
    container.append(span);
  }
}
