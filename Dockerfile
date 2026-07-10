FROM rust:1-slim AS build
WORKDIR /app
COPY Cargo.toml Cargo.lock ./
COPY migrations ./migrations
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/target/release/officesuite-web ./officesuite-web
COPY web ./web
ENV DATABASE_URL=sqlite:///data/officesuite.db
ENV WORKSPACES_DIR=/data/workspaces
ENV PORT=8080
VOLUME ["/data"]
EXPOSE 8080
CMD ["./officesuite-web"]
