#!/usr/bin/env python3
"""
Entry point for the low-power TCP gateway.

Usage::

    python main.py [--config path/to/config.yaml] [--host HOST] [--port PORT]

Environment variables prefixed with ``GATEWAY_`` can override any setting.
"""

import argparse
import asyncio
import logging
import os
import sys


def _parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Low-power TCP gateway for IoT devices"
    )
    parser.add_argument(
        "--config",
        default=os.environ.get("GATEWAY_CONFIG", "config.yaml"),
        help="Path to YAML configuration file (default: config.yaml)",
    )
    parser.add_argument("--host", default=None, help="Bind address override")
    parser.add_argument("--port", type=int, default=None, help="Bind port override")
    parser.add_argument(
        "--log-level",
        default=None,
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Log level override",
    )
    return parser.parse_args(argv)


def _setup_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S",
    )


def main(argv=None) -> int:
    args = _parse_args(argv)

    # Import here so the module can be imported without side-effects.
    from gateway.config import GatewayConfig
    from gateway.router import Router
    from gateway.server import GatewayServer
    from gateway.backends.mqtt import MQTTBackend
    from gateway.backends.http import HTTPBackend

    config = GatewayConfig.from_file(args.config)
    config.apply_env_overrides()

    # CLI overrides take highest priority.
    if args.host is not None:
        config.host = args.host
    if args.port is not None:
        config.port = args.port
    if args.log_level is not None:
        config.log_level = args.log_level

    _setup_logging(config.log_level)
    logger = logging.getLogger(__name__)

    logger.info(
        "Starting low-power TCP gateway on %s:%s", config.host, config.port
    )

    router = Router()
    router.register_backend(MQTTBackend(config.mqtt))
    router.register_backend(HTTPBackend(config.http))

    server = GatewayServer(config, router)

    try:
        asyncio.run(server.run())
    except KeyboardInterrupt:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
