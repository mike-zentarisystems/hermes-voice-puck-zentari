FROM python:3.13-slim-trixie@sha256:e48f59cdba69a3cbd9ed2cf0f348d90d45a8d350d98134cf1070bc106ee9ed90 AS wheels
COPY requirements.lock /tmp/requirements.lock
RUN python -m pip install --no-cache-dir --no-compile --only-binary=:all: --require-hashes \
    --target=/packages -r /tmp/requirements.lock

FROM cgr.dev/chainguard/wolfi-base@sha256:580c9ae79c0229cc381475fbfbed0d91fab5d6606e57cd78732f9836de109e90
RUN apk add --no-cache python-3.13=3.13.18_git20261115-r1 libstdc++=16.2.0-r4 libgomp=16.2.0-r4
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
COPY --from=wheels /packages/ /usr/lib/python3.13/site-packages/
WORKDIR /app
COPY services/voice-gateway/ ./services/voice-gateway/
COPY pwa/ ./pwa/
COPY server.py ./
USER 10001:10001
EXPOSE 8443 8444
ENTRYPOINT ["python", "-B", "server.py"]
