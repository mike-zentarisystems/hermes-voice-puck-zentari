FROM python:3.13-slim@sha256:d98cc29d3266e535dd9da1099688d9cb3ca4753813571e071763d17725460962
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1 OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
WORKDIR /app
COPY requirements.lock ./
RUN pip install --require-hashes --requirement requirements.lock
COPY services/voice-gateway/ ./services/voice-gateway/
COPY pwa/ ./pwa/
COPY server.py ./
USER 10001:10001
EXPOSE 8443 8444
ENTRYPOINT ["python", "-B", "server.py"]
