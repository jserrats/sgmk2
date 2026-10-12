FROM python:3.10-slim

# Logs straight to stdout, so `kubectl logs` shows them as they happen.
ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

WORKDIR /src

# opencv-python-headless bundles FFmpeg (for the camera's MJPEG stream) and needs
# no libGL, so the slim image is enough.
COPY requirements.txt .
RUN pip install -r requirements.txt

COPY . .

USER 1000
EXPOSE 5000
CMD ["python", "sgmk2-cv.py"]
