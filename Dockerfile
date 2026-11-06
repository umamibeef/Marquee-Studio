# Umami Marquee Studio container. The Flask shell (studio/server.py) over the shared marquee_pack toolchain,
# plus the built React SPA. The firmware build is NOT involved: nothing here touches bootloader/ or the firmware.
#
#   dev   : compose.yaml        -> studio-web (vite HMR) + studio-api (this image, 'pydeps' stage, source bind-mounted)
#   prod  : compose.prod.yaml   -> one 'prod' image: npm run build, then gunicorn serves the built dist

# --- python deps (base for the API in dev and prod) ------------------------------------------------
FROM python:3.12-slim AS pydeps
WORKDIR /app
COPY studio/requirements.txt studio/requirements.txt
RUN pip install --no-cache-dir -r studio/requirements.txt
# clang with the BPF backend (mcpu=v4) to compile the Studio's apps (tools/mkapp.py); Debian's clang is too old
RUN apt-get update && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
 && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-19 \
 && rm -rf /var/lib/apt/lists/*
ENV MARQUEE_STUDIO_HOST=0.0.0.0 MARQUEE_STUDIO_PORT=8750 PYTHONUNBUFFERED=1

# --- frontend build (prod only) --------------------------------------------------------------------
FROM node:22-slim AS web-build
WORKDIR /app/frontend
# whole workspace incl. tsconfig.base.json (dockerignore drops node_modules/dist)
COPY frontend/ ./
RUN npm ci
RUN npm run build          # -> /app/frontend/apps/studio/dist

# --- prod: gunicorn serves the built SPA + the API -------------------------------------------------
FROM pydeps AS prod
COPY marquee_pack/ marquee_pack/
COPY third_party/ third_party/
COPY firmware/emoji/ firmware/emoji/
COPY firmware/*.txt firmware/
COPY studio/ studio/
COPY --from=web-build /app/frontend/apps/studio/dist frontend/apps/studio/dist
EXPOSE 8750
# --chdir studio so `import core` and server's relative paths resolve; marquee_studio/__init__ puts /app on sys.path
CMD ["gunicorn", "-b", "0.0.0.0:8750", "-w", "2", "--timeout", "120", "--chdir", "studio", "server:app"]
