FROM alpine:3.18@sha256:de0eb0b3f2a47ba1eb89389859a9bd88b28e82f5826b6969ad604979713c2d4f AS builder
RUN --mount=type=cache,target=/var/cache/apk apk add \
    gcc \
    musl-dev \
    bash \
    binutils \
    upx \
    pkgconf
WORKDIR /app
COPY eat ./
COPY src/ src/
COPY include/ include/
COPY app/ app/
ARG PACK=1
# The driver is the application's choice, not the image's. Install only what
# database.driver asks for, then build the backend against it.
RUN chmod +x eat && \
    driver=$(sed -n 's/^[[:space:]]*database\.driver[[:space:]]*=[[:space:]]*//p' app/application.properties \
             | sed 's/[[:space:]]*#.*//; s/[[:space:]]*$//' | tail -1) && \
    driver=${driver:-sqlite} && \
    echo "building backend for driver: $driver" && \
    case "$driver" in \
        sqlite) \
            apk add --no-cache sqlite-static sqlite-dev; \
            extra_ld="" ;; \
        postgres) \
            apk add --no-cache postgresql-dev openssl-libs-static zlib-static; \
            extra_ld="-lpgcommon -lpgport -lssl -lcrypto -lz" ;; \
        *)        echo "unknown database.driver '$driver'" >&2; exit 1 ;; \
    esac && \
    EXTRA_CFLAGS="-static -Os -fno-asynchronous-unwind-tables -fno-ident" \
    EXTRA_LDFLAGS="-Wl,--gc-sections -s $extra_ld" \
    ./eat build && \
    strip --strip-all .build/server && \
    objcopy --remove-section .eh_frame --remove-section .eh_frame_hdr \
            --remove-section .comment --remove-section .note.gnu.build-id \
            .build/server && \
    if [ "$PACK" = "1" ]; then upx --best --lzma .build/server; fi

FROM scratch
LABEL org.opencontainers.image.title="Gargantua" \
      org.opencontainers.image.description="A web framework for C" \
      org.opencontainers.image.source="https://github.com/signorepesce/Gargantua"
COPY --from=builder /app/.build/server /gargantua
COPY app/application.properties /app/application.properties
ENV SERVER_ADDRESS=0.0.0.0
EXPOSE 8100
CMD ["/gargantua"]
