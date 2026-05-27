# CHAT.COM — Setup Guide

`CHAT.COM` is a CP/M client for OpenAI Chat Completions and any compatible
endpoint (Ollama, llama.cpp server, LM Studio, vLLM, etc.). It talks to the
host through Intel 8080 I/O ports backed by libcurl on the desktop emulator
and by the ESP-IDF HTTP client on the ESP32 firmware.

All configuration is read from the Altair env store via the `ENV` app
(set with `ENV NAME=value`) or, on the device, from the web config portal.
Both sides share the NVS namespace `ENVIRONMENT` on the ESP32, and
`local_altair/altair_env.txt` on the host emulator.

## Required environment variables

### ESP32 firmware (`port_drivers/chat_io.c`)

| Variable | When | Purpose |
|---|---|---|
| `CHAT_PROVIDER`    | always | `openai` (uses `https://api.openai.com/v1/chat/completions`) or `compatible` (uses `CHAT_ENDPOINT`) |
| `CHAT_ENDPOINT`    | `CHAT_PROVIDER=compatible` | Full URL, e.g. `http://192.168.1.20:11434/v1/chat/completions` |
| `CHAT_OPENAI_KEY`  | `CHAT_PROVIDER=openai` (optional for `compatible`) | Bearer token sent in the `Authorization` header |

### Host emulator (`local_altair/PortDrivers/host_chat_io.c`)

The host emulator now honors the same `CHAT_PROVIDER` / `CHAT_ENDPOINT` /
`CHAT_OPENAI_KEY` variables as the ESP32 firmware. The driver picks one URL
at startup (logged as `[Chat] provider=... url=... key=...`). To change
provider/endpoint, edit `local_altair/altair_env.txt` and relaunch
`altair-local`.

### Read by `CHAT.COM` itself (both platforms, via `e_get`)

| Variable | Default | Purpose |
|---|---|---|
| `CHAT_MODEL`       | `gemma3:1b` | Model name passed to the API |
| `CHAT_MAX_TOKENS`  | `1024`      | `max_tokens` in the request |
| `CHAT_TEMPERATURE` | `0.7`       | Sampling temperature |

These can also be set in `CHAT.CFG` on disk A:; env-store values override the
config file when present.

## Example: OpenAI

Set inside CP/M:

```text
A>ENV CHAT_PROVIDER=openai
A>ENV CHAT_OPENAI_KEY=sk-proj-xxxxxxxxxxxxxxxxxxxxxxxx
A>ENV CHAT_MODEL=gpt-4o-mini
A>CHAT
```

The same `CHAT_OPENAI_KEY` name is used on both the ESP32 firmware and the
host emulator (set it in `local_altair/altair_env.txt` for the host).

## Example: Ollama on the LAN

```text
A>ENV CHAT_PROVIDER=compatible
A>ENV CHAT_ENDPOINT=http://192.168.1.20:11434/v1/chat/completions
A>ENV CHAT_MODEL=gemma3:1b
A>CHAT
```

For the Ollama server itself to accept connections from the Altair and use a
useful context window, configure it as below.

---

# Ollama Environment Configuration Guide

This guide explains how to install Ollama on Linux and configure environment variables for the server (Systemd) to enable network access and a larger context window.

## 1. Install Ollama on Linux

The official install script downloads the latest Ollama release, installs the `ollama` binary, and registers it as a systemd service.

Run the following in your terminal:

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

Verify the installation:

```bash
ollama --version
systemctl status ollama
```

> **Note:** The installer creates an `ollama` system user and enables the `ollama.service` unit so the server starts automatically on boot.

## 2. Edit the Service Configuration

Use the `systemctl edit` command to create a persistent override for the Ollama service. This ensures your settings are not lost when Ollama is updated.

Run the following command in your terminal:

```bash
sudo systemctl edit ollama.service
```

## 3. Add Environment Variables

An editor will open (usually Nano). Paste the following block exactly as shown:

```ini
[Service]
Environment="OLLAMA_HOST=0.0.0.0"
Environment="OLLAMA_CONTEXT_LENGTH=8192"
```

- **To Save:** Press `Ctrl + O`, then `Enter`.
- **To Exit:** Press `Ctrl + X`.

### Key Settings

- **`OLLAMA_HOST=0.0.0.0`** — Allows the server to accept connections from any device on your local network (e.g., your laptop or tablet).
- **`OLLAMA_CONTEXT_LENGTH=8192`** — Sets the default context window to 8,192 tokens for all models.

## 4. Restart the Service

To apply the changes, you must reload the systemd manager configuration and restart the service:

```bash
# Reload systemd manager configuration
sudo systemctl daemon-reload

# Restart the Ollama service
sudo systemctl restart ollama
```

## 5. Verify Success

To verify that the settings have been applied correctly, check the status of the service:

```bash
systemctl status ollama
```

You can also verify the network listener is active on all interfaces:

```bash
netstat -tulpn | grep 11434
```

If successful, you will see `0.0.0.0:11434` in the output.
