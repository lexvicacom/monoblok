FROM cgr.dev/chainguard/glibc-dynamic:latest

COPY zig-out/bin/monoblok /usr/local/bin/monoblok
COPY patchbay.edn /etc/monoblok/patchbay.edn

EXPOSE 4222

USER 65532:65532
ENTRYPOINT ["/usr/local/bin/monoblok"]
CMD ["--port", "4222", "--patchbay", "/etc/monoblok/patchbay.edn"]
