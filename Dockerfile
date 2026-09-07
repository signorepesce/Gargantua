FROM alpine:3.18@sha256:de0eb0b3f2a47ba1eb89389859a9bd88b28e82f5826b6969ad604979713c2d4f AS builder
RUN --mount=type=cache,target=/var/cache/apk apk add \
    gcc \
    musl-dev \
    bash \
    binutils \
    upx \
    sqlite-static \
    sqlite-dev
WORKDIR /app
COPY eat ./
COPY src/ src/
COPY include/ include/
COPY app/ app/
ARG PACK=1
RUN chmod +x eat && \
    EXTRA_CFLAGS="-static -Os -fno-asynchronous-unwind-tables -fno-ident" \
    EXTRA_LDFLAGS="-Wl,--gc-sections -s" \
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
ENV SERVER_ADDRESS=0.0.0.0 \
    DATABASE_URL=/data/gargantua.db
VOLUME /data
EXPOSE 8100
CMD ["/gargantua"]
