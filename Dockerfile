FROM rust:1-slim-bookworm AS build
WORKDIR /app
COPY Cargo.toml Cargo.lock ./
COPY migrations ./migrations
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
      chromium fonts-liberation fonts-noto-color-emoji fontconfig ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app/target/release/lectern-web ./lectern-web
COPY web ./web
# Custom fonts for PDF export (headless Chromium, see src/export.rs) — TTF/OTF
# only, fontconfig can't use WOFF/WOFF2. Baked into the image and cached at
# build time so PDF export doesn't depend on the host's font/fontconfig state,
# which a bind mount + host-side `fc-cache` never actually reaches (the
# container has its own fontconfig cache; `fc-cache` must run where the fonts
# are read from). Empty by default — add font files under fonts/ to use this.
COPY fonts/ /usr/local/share/fonts/custom/
RUN fc-cache -f
ENV DATABASE_URL=sqlite:///data/officesuite.db
ENV WORKSPACES_DIR=/data/workspaces
ENV PORT=8080
ENV CHROME=/usr/bin/chromium
VOLUME ["/data"]
EXPOSE 8080
CMD ["./lectern-web"]
