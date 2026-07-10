FROM rust:1-slim-bookworm AS build
WORKDIR /app
COPY Cargo.toml Cargo.lock ./
COPY migrations ./migrations
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends \
      chromium fonts-liberation fonts-noto-color-emoji ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /app/target/release/officesuite-web ./officesuite-web
COPY web ./web
ENV DATABASE_URL=sqlite:///data/officesuite.db
ENV WORKSPACES_DIR=/data/workspaces
ENV PORT=8080
ENV CHROME=/usr/bin/chromium
VOLUME ["/data"]
EXPOSE 8080
CMD ["./officesuite-web"]
