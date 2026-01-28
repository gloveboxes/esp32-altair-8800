# WebSocket Connection Fixes

## Summary

This document describes the fixes applied to resolve WebSocket connection issues in the Altair 8800 ESP32 emulator's web terminal interface.

## Problems Identified

### 1. Two-Click Connection Bug

**Symptom:** Users had to click the "Connect" button twice to establish a WebSocket connection.

**Root Cause:** A race condition in the JavaScript WebSocket event handlers. When a new WebSocket connection was created:

1. The old WebSocket's `onclose` event was already queued in the JavaScript event loop
2. The new WebSocket was created and assigned to `state.ws`
3. The queued `onclose` from the OLD WebSocket fired and set `state.ws = null`
4. This destroyed the reference to the NEW WebSocket
5. The second click would then create a fresh connection that worked

**Fix:** Capture the WebSocket reference when setting up handlers and validate it in each callback:

```javascript
function setupWebSocketEventHandlers(altairAddress) {
    // Capture reference to THIS WebSocket instance
    const thisWs = state.ws;

    state.ws.onopen = () => {
        // Ignore if this is not the current WebSocket
        if (state.ws !== thisWs) {
            console.log("Ignoring onopen from old WebSocket");
            return;
        }
        // ... handle connection
    };

    state.ws.onclose = (event) => {
        // CRITICAL: Only process if this is still the current WebSocket
        if (state.ws !== thisWs) {
            console.log("Ignoring onclose from old WebSocket");
            return;
        }
        // ... handle close
    };
}
```

### 2. Grayed-Out Connect Button

**Symptom:** The Connect button would become disabled and stay grayed out indefinitely if the ESP32 was unreachable.

**Root Cause:** The button was disabled during the `WebSocket.CONNECTING` state, but WebSocket connections have no built-in timeout. If the server never responds, the connection attempt hangs forever.

**Fix:** Added a 5-second connection timeout:

```javascript
// In state object
connectTimeout: null,

// In CONFIG
CONNECTION_TIMEOUT: 5000  // 5 second connection timeout

// In attemptConnection()
state.connectTimeout = setTimeout(() => {
    if (state.ws && state.ws.readyState === WebSocket.CONNECTING) {
        console.log("Connection timeout - aborting");
        state.ws.close();
        state.ws = null;
        state.connected = false;
        showMessage('Connection timeout', true);
        updateReconnectButton();
        scheduleReconnect();
    }
}, CONFIG.CONNECTION_TIMEOUT);
```

The timeout is cancelled when:
- Connection succeeds (`onopen` handler)
- Connection is manually closed (`closeConnection()`)
- A new connection attempt starts (`attemptConnection()`)

### 3. Button State Not Reflecting Connection Status

**Symptom:** The button text and enabled state didn't properly reflect whether a connection was in progress.

**Fix:** Updated `updateReconnectButton()` to handle three states:

```javascript
function updateReconnectButton() {
    const isOpen = state.connected && state.ws && state.ws.readyState === WebSocket.OPEN;
    const isConnecting = state.ws && state.ws.readyState === WebSocket.CONNECTING;
    
    elements.reconnectBtn.disabled = isConnecting;  // Only disable while connecting
    
    if (isConnecting) {
        elements.reconnectBtn.textContent = `Connecting to ${hostText}...`;
    } else if (isOpen) {
        elements.reconnectBtn.textContent = `Disconnect from ${hostText}`;
    } else {
        elements.reconnectBtn.textContent = `Connect to ${hostText}`;
    }
}
```

## Files Modified

| File | Changes |
|------|---------|
| `terminal/index.html` | Added connection timeout, fixed race condition in event handlers, improved button state management |
| `terminal/static_html_hex.h` | Regenerated from index.html (auto-generated) |

## Key Concepts

### JavaScript Event Loop Race Conditions

Even after setting `ws.onclose = null`, any already-queued `onclose` events will still fire with the original handler closure. This is because JavaScript event handlers are queued asynchronously. The fix is to capture the WebSocket reference at handler setup time and validate it when the event fires.

### WebSocket Connection States

WebSocket has four `readyState` values:
- `CONNECTING (0)` - Connection in progress
- `OPEN (1)` - Connected and ready
- `CLOSING (2)` - Close in progress  
- `CLOSED (3)` - Connection closed

The browser provides no timeout for the CONNECTING state - you must implement your own.

## Testing

To verify the fixes:

1. **Two-click test:** Click Connect once - should connect immediately
2. **Timeout test:** Disconnect ESP32 from network, click Connect - button should re-enable after 5 seconds with "Connection timeout" message
3. **Reconnect test:** With ESP32 connected, disconnect and reconnect - should auto-reconnect

## Version

These fixes were applied on January 28, 2026.
