FROM python:3.13-slim
WORKDIR /app
COPY server.py /app/server.py
USER 65532:65532
EXPOSE 8765
CMD ["python", "-u", "/app/server.py"]
