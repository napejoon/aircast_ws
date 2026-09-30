# The signalling server as a container, for a host that already runs nginx
# (TLS) and coturn natively. One file, one dependency, nothing written to disk.
FROM python:3.12-slim
# Pinned to what the systemd deployment ran, so the move changes the packaging
# and not the library under it. CI tests against requirements.txt (>=13).
RUN pip install --no-cache-dir "websockets==17.1" \
 && useradd --system --no-create-home --shell /usr/sbin/nologin aircast
COPY aircast_signal.py /app/aircast_signal.py
USER aircast
CMD ["python", "/app/aircast_signal.py"]
