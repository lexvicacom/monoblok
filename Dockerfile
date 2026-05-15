FROM cgr.dev/chainguard/wolfi-base:latest

RUN apk add --no-cache openssl

ARG MONOBLOK_BIN=build-package/monoblok

COPY ${MONOBLOK_BIN} /usr/local/bin/monoblok
COPY patchbay.edn /etc/monoblok/patchbay.edn

EXPOSE 4222

USER 65532:65532
ENTRYPOINT ["/usr/local/bin/monoblok"]
CMD ["--port", "4222", "--patchbay", "/etc/monoblok/patchbay.edn"]
