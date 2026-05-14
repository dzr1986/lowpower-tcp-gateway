FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libasio-dev \
    libmosquitto-dev \
    nlohmann-json3-dev \
    libssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN cmake -S . -B build && \
    cmake --build build -j

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    libmosquitto1 \
    libssl3 \
    libhiredis0.14 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/lowpower_tcp_gateway /app/lowpower_tcp_gateway
COPY config.json /app/config.json

EXPOSE 9000
EXPOSE 8080

CMD ["/app/lowpower_tcp_gateway", "/app/config.json"]