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
