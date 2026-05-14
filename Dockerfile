FROM python:3.11-slim

LABEL maintainer="lowpower-tcp-gateway"
LABEL description="Low-power TCP gateway for IoT devices"

WORKDIR /app

# Install dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source
COPY gateway/ ./gateway/
COPY main.py .
COPY config.yaml .

# Expose TCP port
EXPOSE 9000

# Health check – just verify the process is up
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD python -c "import socket; s=socket.socket(); s.settimeout(2); s.connect(('localhost', 9000)); s.close()"

# Default command
CMD ["python", "main.py", "--config", "config.yaml"]
