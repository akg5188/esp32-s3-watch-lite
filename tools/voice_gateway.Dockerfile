FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    HOME=/cache \
    XDG_CACHE_HOME=/cache \
    HF_HOME=/cache/huggingface \
    WATCH_VOICE_HOST=0.0.0.0 \
    WATCH_VOICE_PORT=8790

WORKDIR /app

COPY tools/voice_gateway.requirements.txt /tmp/requirements.txt
RUN pip install -r /tmp/requirements.txt

COPY tools/voice_gateway.py /app/tools/voice_gateway.py
COPY main/watch_ai_cn_24.c /app/main/watch_ai_cn_24.c

RUN mkdir -p /app/tools/watch_gateway /cache \
    && chmod 0777 /app/tools/watch_gateway /cache

USER 1000:1000
EXPOSE 8790

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8790/health', timeout=5).read()" || exit 1

CMD ["python", "/app/tools/voice_gateway.py"]
