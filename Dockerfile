# ── Stage 1: Builder ─────────────────────────────────────────
FROM python:3.11-slim AS builder

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir --prefix=/install -r requirements.txt

# ── Stage 2: Runtime ─────────────────────────────────────────
FROM python:3.11-slim

# HF Spaces runs as user 1000
RUN useradd -m -u 1000 appuser

WORKDIR /app

# Copy dependencies
COPY --from=builder /install /usr/local

# Copy source
COPY --chown=appuser:appuser . .

USER appuser

# HF Spaces exposes port 7860
EXPOSE 7860

# Start with uvicorn, binding to 0.0.0.0:7860
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "7860", "--workers", "1", "--ws", "websockets"]
